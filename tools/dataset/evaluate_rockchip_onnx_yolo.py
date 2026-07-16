#!/usr/bin/env python3
"""Evaluate a Rockchip nine-output YOLOv8 ONNX on a YOLO-format split."""

from __future__ import annotations

import argparse
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort


CLASS_NAMES = ("cell phone", "cup", "keyboard", "mouse", "laptop", "book")
IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


def box_iou(a: np.ndarray, b: np.ndarray) -> float:
    x1, y1 = np.maximum(a[:2], b[:2])
    x2, y2 = np.minimum(a[2:], b[2:])
    inter = max(float(x2 - x1), 0.0) * max(float(y2 - y1), 0.0)
    area_a = max(float(a[2] - a[0]), 0.0) * max(float(a[3] - a[1]), 0.0)
    area_b = max(float(b[2] - b[0]), 0.0) * max(float(b[3] - b[1]), 0.0)
    return inter / (area_a + area_b - inter + 1e-9)


def nms(detections: list[dict], threshold: float) -> list[dict]:
    kept: list[dict] = []
    for det in sorted(detections, key=lambda item: item["score"], reverse=True):
        if all(
            det["class_id"] != prior["class_id"]
            or box_iou(det["box"], prior["box"]) <= threshold
            for prior in kept
        ):
            kept.append(det)
    return kept


def preprocess(image: np.ndarray, size: int, mode: str) -> tuple[np.ndarray, dict]:
    height, width = image.shape[:2]
    if mode == "stretch":
        resized = cv2.resize(image, (size, size), interpolation=cv2.INTER_LINEAR)
        transform = {"scale_x": size / width, "scale_y": size / height, "pad_x": 0.0, "pad_y": 0.0}
    else:
        scale = min(size / width, size / height)
        new_width = int(round(width * scale))
        new_height = int(round(height * scale))
        resized_inner = cv2.resize(image, (new_width, new_height), interpolation=cv2.INTER_LINEAR)
        pad_x = (size - new_width) / 2.0
        pad_y = (size - new_height) / 2.0
        left = int(round(pad_x - 0.1))
        right = int(round(pad_x + 0.1))
        top = int(round(pad_y - 0.1))
        bottom = int(round(pad_y + 0.1))
        resized = cv2.copyMakeBorder(
            resized_inner, top, bottom, left, right, cv2.BORDER_CONSTANT, value=(114, 114, 114)
        )
        transform = {"scale_x": scale, "scale_y": scale, "pad_x": float(left), "pad_y": float(top)}

    rgb = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB)
    tensor = np.transpose(rgb.astype(np.float32) / 255.0, (2, 0, 1))[None]
    return tensor, transform


def decode(
    outputs: list[np.ndarray],
    transform: dict,
    original_width: int,
    original_height: int,
    conf_threshold: float,
    nms_threshold: float,
) -> list[dict]:
    if len(outputs) != 9:
        raise ValueError(f"expected 9 outputs, got {len(outputs)}")

    candidates: list[dict] = []
    for branch in range(3):
        boxes = outputs[branch * 3][0]
        scores = outputs[branch * 3 + 1][0]
        sums = outputs[branch * 3 + 2][0, 0]
        if scores.shape[0] != len(CLASS_NAMES):
            raise ValueError(f"expected {len(CLASS_NAMES)} classes, got {scores.shape[0]}")
        grid_height, grid_width = boxes.shape[1:]
        stride = 640 // grid_height
        dfl_length = boxes.shape[0] // 4

        rows, cols = np.where(sums >= conf_threshold)
        for row, col in zip(rows.tolist(), cols.tolist()):
            class_id = int(np.argmax(scores[:, row, col]))
            score = float(scores[class_id, row, col])
            if score < conf_threshold:
                continue
            distances = []
            for side in range(4):
                logits = boxes[side * dfl_length : (side + 1) * dfl_length, row, col]
                probabilities = np.exp(logits - np.max(logits))
                probabilities /= probabilities.sum()
                distances.append(float(np.dot(probabilities, np.arange(dfl_length))))

            model_box = np.array(
                [
                    (col + 0.5 - distances[0]) * stride,
                    (row + 0.5 - distances[1]) * stride,
                    (col + 0.5 + distances[2]) * stride,
                    (row + 0.5 + distances[3]) * stride,
                ],
                dtype=np.float32,
            )
            model_box[[0, 2]] = (model_box[[0, 2]] - transform["pad_x"]) / transform["scale_x"]
            model_box[[1, 3]] = (model_box[[1, 3]] - transform["pad_y"]) / transform["scale_y"]
            model_box[[0, 2]] = np.clip(model_box[[0, 2]], 0, original_width)
            model_box[[1, 3]] = np.clip(model_box[[1, 3]], 0, original_height)
            candidates.append({"class_id": class_id, "score": score, "box": model_box})

    return nms(candidates, nms_threshold)


def load_ground_truth(label_path: Path, width: int, height: int) -> list[dict]:
    ground_truth: list[dict] = []
    if not label_path.exists():
        return ground_truth
    for line in label_path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        class_id_text, cx_text, cy_text, width_text, height_text = line.split()
        class_id = int(class_id_text)
        cx, cy, box_width, box_height = map(float, (cx_text, cy_text, width_text, height_text))
        ground_truth.append(
            {
                "class_id": class_id,
                "box": np.array(
                    [
                        (cx - box_width / 2) * width,
                        (cy - box_height / 2) * height,
                        (cx + box_width / 2) * width,
                        (cy + box_height / 2) * height,
                    ],
                    dtype=np.float32,
                ),
            }
        )
    return ground_truth


def metrics(ground_truth: dict[str, list[dict]], predictions: dict[str, list[dict]], iou_threshold: float):
    per_class = []
    for class_id, class_name in enumerate(CLASS_NAMES):
        gt_by_image: dict[str, list[dict]] = {}
        for image_name, items in ground_truth.items():
            matches = [item for item in items if item["class_id"] == class_id]
            gt_by_image[image_name] = [{"box": item["box"], "matched": False} for item in matches]
        gt_count = sum(len(items) for items in gt_by_image.values())
        class_predictions = sorted(
            [
                (image_name, item)
                for image_name, items in predictions.items()
                for item in items
                if item["class_id"] == class_id
            ],
            key=lambda pair: pair[1]["score"],
            reverse=True,
        )
        true_positive = np.zeros(len(class_predictions), dtype=np.float32)
        false_positive = np.zeros(len(class_predictions), dtype=np.float32)
        for index, (image_name, prediction) in enumerate(class_predictions):
            available = [item for item in gt_by_image.get(image_name, []) if not item["matched"]]
            if not available:
                false_positive[index] = 1
                continue
            overlaps = [box_iou(prediction["box"], item["box"]) for item in available]
            best_index = int(np.argmax(overlaps))
            if overlaps[best_index] >= iou_threshold:
                true_positive[index] = 1
                available[best_index]["matched"] = True
            else:
                false_positive[index] = 1

        tp = np.cumsum(true_positive)
        fp = np.cumsum(false_positive)
        recall = tp / max(gt_count, 1)
        precision = tp / np.maximum(tp + fp, 1e-9)
        if gt_count:
            metric_recall = np.concatenate(([0.0], recall, [1.0]))
            metric_precision = np.concatenate(([1.0], precision, [0.0]))
            metric_precision = np.maximum.accumulate(metric_precision[::-1])[::-1]
            changes = np.where(metric_recall[1:] != metric_recall[:-1])[0]
            average_precision = float(
                np.sum((metric_recall[changes + 1] - metric_recall[changes]) * metric_precision[changes + 1])
            )
        else:
            average_precision = 0.0
        per_class.append(
            {
                "class": class_name,
                "gt": gt_count,
                "pred": len(class_predictions),
                "precision": float(precision[-1]) if len(precision) else 0.0,
                "recall": float(recall[-1]) if len(recall) else 0.0,
                "ap50": average_precision,
            }
        )
    return per_class


def evaluate(session: ort.InferenceSession, dataset: Path, mode: str, args: argparse.Namespace) -> None:
    image_dir = dataset / args.split / "images"
    label_dir = dataset / args.split / "labels"
    images = sorted(path for path in image_dir.iterdir() if path.suffix.lower() in IMAGE_SUFFIXES)
    ground_truth: dict[str, list[dict]] = {}
    predictions: dict[str, list[dict]] = {}
    input_name = session.get_inputs()[0].name

    for image_path in images:
        image = cv2.imread(str(image_path))
        if image is None:
            raise ValueError(f"failed to read {image_path}")
        height, width = image.shape[:2]
        tensor, transform = preprocess(image, args.imgsz, mode)
        outputs = session.run(None, {input_name: tensor})
        predictions[image_path.name] = decode(
            outputs, transform, width, height, args.conf_threshold, args.nms_threshold
        )
        ground_truth[image_path.name] = load_ground_truth(label_dir / f"{image_path.stem}.txt", width, height)

    per_class = metrics(ground_truth, predictions, args.eval_iou_threshold)
    mean_ap = float(np.mean([item["ap50"] for item in per_class]))
    empty_images = sum(not items for items in predictions.values())
    print(f"\n[{mode}] images={len(images)} predictions={sum(map(len, predictions.values()))} empty={empty_images}")
    print(f"{'class':<12} {'GT':>5} {'Pred':>5} {'P':>9} {'R':>9} {'AP50':>9}")
    for item in per_class:
        print(
            f"{item['class']:<12} {item['gt']:>5} {item['pred']:>5} "
            f"{item['precision']:>8.2%} {item['recall']:>8.2%} {item['ap50']:>8.2%}"
        )
    print(f"mAP@0.5: {mean_ap:.2%}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--onnx", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--split", choices=("val", "test"), default="test")
    parser.add_argument("--preprocess", choices=("both", "letterbox", "stretch"), default="both")
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--conf-threshold", type=float, default=0.25)
    parser.add_argument("--nms-threshold", type=float, default=0.45)
    parser.add_argument("--eval-iou-threshold", type=float, default=0.5)
    args = parser.parse_args()
    if args.imgsz != 640:
        raise ValueError("the current Rockchip export has a fixed 640x640 input")
    session = ort.InferenceSession(str(args.onnx), providers=["CPUExecutionProvider"])
    modes = ("letterbox", "stretch") if args.preprocess == "both" else (args.preprocess,)
    for mode in modes:
        evaluate(session, args.dataset, mode, args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
