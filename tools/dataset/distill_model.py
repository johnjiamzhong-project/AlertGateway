import torch
import torch.nn.functional as F
from ultralytics.nn.tasks import DetectionModel


def raw_scales(model, image):
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


def distill_loss(student_raw, teacher_raw, temperature, box_weight, class_weight, feature_weight):
    if len(student_raw) != len(teacher_raw):
        raise RuntimeError("teacher and student have different detection scale counts")
    total = student_raw[0].new_zeros(())
    for student, teacher in zip(student_raw, teacher_raw):
        if student.shape != teacher.shape:
            raise RuntimeError(f"teacher/student raw shapes differ: {tuple(teacher.shape)} vs {tuple(student.shape)}")
        teacher = teacher.detach()
        box = F.smooth_l1_loss(student[:, :64], teacher[:, :64])
        student_cls = student[:, 64:] / temperature
        teacher_cls = teacher[:, 64:] / temperature
        cls = F.mse_loss(student_cls.sigmoid(), teacher_cls.sigmoid()) * (temperature**2)
        feature = F.mse_loss(student, teacher)
        total = total + box_weight * box + class_weight * cls + feature_weight * feature
    return total / len(student_raw)


class DistillDetectionModel(DetectionModel):
    """Checkpoint-safe YOLO model with an optional non-serialized teacher."""

    def attach_teacher(self, teacher, temperature, box_weight, class_weight, feature_weight):
        object.__setattr__(self, "_distill_teacher", teacher)
        self._distill_temperature = temperature
        self._distill_box_weight = box_weight
        self._distill_class_weight = class_weight
        self._distill_feature_weight = feature_weight

    def forward(self, x, *forward_args, **forward_kwargs):
        if not isinstance(x, dict):
            output = super().forward(x, *forward_args, **forward_kwargs)
            if not hasattr(self, "_distill_teacher") and self.training:
                return output
            if isinstance(output, dict):
                output = output.get("one2one") or output.get("one2many") or next(iter(output.values()))
            if isinstance(output, tuple) and len(output) == 2 and torch.is_tensor(output[0]):
                output = output[0]
            return output

        student_loss_raw, student_raw = raw_scales(self, x["img"])
        base_loss, loss_items = self.loss(x, student_loss_raw)
        with torch.no_grad():
            _, teacher_raw = raw_scales(self._distill_teacher, x["img"])
        extra = distill_loss(
            student_raw,
            teacher_raw,
            self._distill_temperature,
            self._distill_box_weight,
            self._distill_class_weight,
            self._distill_feature_weight,
        )
        return base_loss + extra, loss_items
