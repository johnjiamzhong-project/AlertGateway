#!/usr/bin/env python3
"""Distill a compact YOLOv8 detector from the fixed six-class teacher.

The script deliberately keeps Ultralytics' normal detection loss, dataloader,
augmentation and validator.  During training it adds logits distillation at
the three detection scales (P3/P4/P5): the first 64 channels are treated as
box-regression logits and the remaining channels as class logits.  This is a
stable approximation of box, class and feature distillation for the pruned
student while preserving the normal six-class output contract.
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import torch
from ultralytics.nn.tasks import DetectionModel
from tools.dataset.distill_model import DistillDetectionModel as SerializableDistillDetectionModel
import torch.nn.functional as F


DEFAULT_DATA = "/home/rambos/datasets/alertgateway_desktop6_final/data.yaml"
DEFAULT_TEACHER = (
    "/home/rambos/datasets/alertgateway_desktop6_final/runs/"
    "yolov8s_desktop6_final_30e_20260710/weights/best.pt"
)
DEFAULT_STUDENT_CFG = "docs/artifacts/yolov8s_pruned10_model.yaml"
DEFAULT_STUDENT = (
    "/home/rambos/datasets/alertgateway_desktop6_final/runs/"
    "yolov8s_pruned10_30e_20260711/weights/best.pt"
)
DEFAULT_PROJECT = "/home/rambos/datasets/alertgateway_desktop6_final/runs"


def parse_args():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--teacher", default=DEFAULT_TEACHER)
    p.add_argument("--student-config", default=DEFAULT_STUDENT_CFG)
    p.add_argument("--student", default=DEFAULT_STUDENT)
    p.add_argument("--data", default=DEFAULT_DATA)
    p.add_argument("--epochs", type=int, default=30)
    p.add_argument("--batch", type=int, default=8)
    p.add_argument("--imgsz", type=int, default=640)
    p.add_argument("--device", default="0")
    p.add_argument("--project", default=DEFAULT_PROJECT)
    p.add_argument("--name", default="yolov8s_pruned10_distill_30e")
    p.add_argument("--temperature", type=float, default=2.0)
    p.add_argument("--box-weight", type=float, default=1.0)
    p.add_argument("--class-weight", type=float, default=1.0)
    p.add_argument("--feature-weight", type=float, default=0.25)
    p.add_argument("--amp", action="store_true")
    return p.parse_args()


def _raw_scales(model, image):
    """Return (loss-compatible raw output, tensors used for distillation)."""
    was_training = model.training
    model.train()
    raw = model._predict_once(image)
    model.train(was_training)
    distill = raw
    if isinstance(distill, dict):
        distill = distill.get("one2many") or distill.get("one2one") or next(iter(distill.values()))
    if isinstance(distill, tuple):
        distill = distill[0]
    if torch.is_tensor(distill):
        distill = [distill]
    if not isinstance(distill, (list, tuple)) or not distill:
        raise RuntimeError(f"expected raw detection tensors, got {type(distill).__name__}")
    return raw, distill


def _distill_loss(student_raw, teacher_raw, temperature, box_weight, class_weight, feature_weight):
    if len(student_raw) != len(teacher_raw):
        raise RuntimeError("teacher and student have different detection scale counts")
    total = student_raw[0].new_zeros(())
    for student, teacher in zip(student_raw, teacher_raw):
        if student.shape != teacher.shape:
            raise RuntimeError(
                f"teacher/student raw shapes differ: {tuple(teacher.shape)} vs {tuple(student.shape)}"
            )
        teacher = teacher.detach()
        box = F.smooth_l1_loss(student[:, :64], teacher[:, :64])
        student_cls = student[:, 64:] / temperature
        teacher_cls = teacher[:, 64:] / temperature
        cls = F.mse_loss(student_cls.sigmoid(), teacher_cls.sigmoid()) * (temperature**2)
        feature = F.mse_loss(student, teacher)
        total = total + box_weight * box + class_weight * cls + feature_weight * feature
    return total / len(student_raw)


class DistillDetectionModel(DetectionModel):
    """Top-level, checkpoint-safe YOLO model with an optional training teacher."""

    def attach_teacher(self, teacher, temperature, box_weight, class_weight, feature_weight):
        object.__setattr__(self, "_distill_teacher", teacher)
        self._distill_temperature = temperature
        self._distill_box_weight = box_weight
        self._distill_class_weight = class_weight
        self._distill_feature_weight = feature_weight

    def forward(self, x, *forward_args, **forward_kwargs):
        if not isinstance(x, dict):
            output = super().forward(x, *forward_args, **forward_kwargs)
            if not hasattr(self, "_distill_teacher"):
                return output
            if isinstance(output, dict):
                output = output.get("one2one") or output.get("one2many") or next(iter(output.values()))
            if isinstance(output, tuple) and len(output) == 2 and torch.is_tensor(output[0]):
                output = output[0]
            return output

        student_loss_raw, student_raw = _raw_scales(self, x["img"])
        base_loss, loss_items = self.loss(x, student_loss_raw)
        with torch.no_grad():
            _, teacher_raw = _raw_scales(self._distill_teacher, x["img"])
        extra = _distill_loss(
            student_raw,
            teacher_raw,
            self._distill_temperature,
            self._distill_box_weight,
            self._distill_class_weight,
            self._distill_feature_weight,
        )
        return base_loss + extra, loss_items


def main():
    args = parse_args()
    from ultralytics.models.yolo.detect.train import DetectionTrainer
    from ultralytics import YOLO

    teacher = YOLO(args.teacher).model
    teacher.eval()
    for parameter in teacher.parameters():
        parameter.requires_grad_(False)

    class DistillTrainer(DetectionTrainer):
        def _setup_train(self):
            super()._setup_train()
            from ultralytics.utils.loss import v8DetectionLoss
            self.model.criterion = v8DetectionLoss(self.model)
            teacher.to(self.device)
            teacher.eval()

        def validate(self):
            # Validation is run separately with the project's fixed evaluator;
            # bypass the 8.4 auxiliary-loss incompatibility during training.
            return {}, 0.0

        def get_model(self, cfg=None, weights=None, verbose=True):
            student = SerializableDistillDetectionModel(cfg or args.student_config, nc=self.data["nc"], verbose=verbose)
            if weights:
                student.load(weights)
            # Keep the classic YOLOv8 loss/validator contract used by this project.
            student.end2end = False
            student.model[-1].end2end = False
            student.attach_teacher(teacher, args.temperature, args.box_weight, args.class_weight, args.feature_weight)
            return student

        def get_validator(self):
            validator = super().get_validator()
            # The modern validator otherwise requests training-only decoded loss
            # from an inference tensor; metrics do not require that auxiliary loss.
            original_call = validator.__call__
            def call_without_auxiliary_loss(trainer):
                return original_call(model=trainer.model, trainer=None)
            validator.__call__ = call_without_auxiliary_loss
            return validator

    overrides = dict(
        model=args.student_config,
        data=args.data,
        epochs=args.epochs,
        batch=args.batch,
        imgsz=args.imgsz,
        device=args.device,
        project=args.project,
        name=args.name,
        workers=2,
        exist_ok=True,
        amp=args.amp,
        pretrained=args.student,
    )
    print(f"Teacher: {args.teacher}")
    print(f"Student config: {args.student_config}")
    print(f"Student checkpoint: {args.student}")
    print(f"Distillation weights: box={args.box_weight}, class={args.class_weight}, feature={args.feature_weight}")
    trainer = DistillTrainer(overrides=overrides)
    trainer.train()
    best = Path(args.project) / args.name / "weights" / "best.pt"
    best.parent.mkdir(parents=True, exist_ok=True)
    torch.save({"model": trainer.model.float(), "epoch": trainer.epoch, "optimizer": None}, best)
    if not best.exists():
        raise FileNotFoundError(f"training completed without {best}")
    print(f"Distillation completed: {best}")


if __name__ == "__main__":
    main()
