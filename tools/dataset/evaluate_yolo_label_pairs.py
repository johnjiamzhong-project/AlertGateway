#!/usr/bin/env python3
"""Compare two YOLO label directories at one fixed prediction threshold.

Ground-truth and prediction files use the same image stem. A greedy, per-class
IoU match is used, so this reports TP/FP/FN, precision, recall, F1, and mean
IoU for matched boxes; it intentionally does not claim mAP because prediction
confidence values are not present in YOLO label files.
"""

import argparse
from collections import defaultdict
from pathlib import Path


CLASS_NAMES = ("cell phone", "cup", "keyboard", "mouse", "laptop", "book")


def read_boxes(directory: Path, stem: str):
    path = directory / f"{stem}.txt"
    if not path.exists():
        return []
    boxes = []
    for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        parts = line.split()
        if len(parts) != 5:
            raise ValueError(f"{path}:{line_no}: expected 5 values")
        class_id = int(parts[0])
        cx, cy, width, height = map(float, parts[1:])
        if class_id < 0 or class_id >= len(CLASS_NAMES) or width <= 0 or height <= 0:
            raise ValueError(f"{path}:{line_no}: invalid YOLO box")
        boxes.append((class_id, cx - width / 2, cy - height / 2, cx + width / 2, cy + height / 2))
    return boxes


def iou(a, b):
    left, top = max(a[1], b[1]), max(a[2], b[2])
    right, bottom = min(a[3], b[3]), min(a[4], b[4])
    intersection = max(0.0, right - left) * max(0.0, bottom - top)
    if intersection <= 0.0:
        return 0.0
    area_a = max(0.0, a[3] - a[1]) * max(0.0, a[4] - a[2])
    area_b = max(0.0, b[3] - b[1]) * max(0.0, b[4] - b[2])
    return intersection / (area_a + area_b - intersection + 1e-12)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ground-truth", type=Path, required=True)
    parser.add_argument("--predictions", type=Path, required=True)
    parser.add_argument("--images", type=Path, required=True)
    parser.add_argument("--iou", type=float, default=0.50)
    args = parser.parse_args()
    if not 0.0 < args.iou <= 1.0:
        raise ValueError("--iou must be in (0, 1]")

    counts = defaultdict(lambda: {"tp": 0, "fp": 0, "fn": 0, "ious": []})
    image_paths = sorted(args.images.glob("*.jpg"))
    for image_path in image_paths:
        gt = read_boxes(args.ground_truth, image_path.stem)
        predictions = read_boxes(args.predictions, image_path.stem)
        matched_gt = set()
        for prediction in predictions:
            best_index, best_iou = -1, 0.0
            for index, target in enumerate(gt):
                if index in matched_gt or target[0] != prediction[0]:
                    continue
                overlap = iou(prediction, target)
                if overlap > best_iou:
                    best_index, best_iou = index, overlap
            if best_index >= 0 and best_iou >= args.iou:
                counts[prediction[0]]["tp"] += 1
                counts[prediction[0]]["ious"].append(best_iou)
                matched_gt.add(best_index)
            else:
                counts[prediction[0]]["fp"] += 1
        for index, target in enumerate(gt):
            if index not in matched_gt:
                counts[target[0]]["fn"] += 1

    print(f"images={len(image_paths)} iou_threshold={args.iou:.2f}")
    total = {"tp": 0, "fp": 0, "fn": 0, "ious": []}
    for class_id, name in enumerate(CLASS_NAMES):
        item = counts[class_id]
        total["tp"] += item["tp"]; total["fp"] += item["fp"]; total["fn"] += item["fn"]
        total["ious"].extend(item["ious"])
        precision = item["tp"] / max(1, item["tp"] + item["fp"])
        recall = item["tp"] / max(1, item["tp"] + item["fn"])
        f1 = 2 * precision * recall / max(1e-12, precision + recall)
        mean_iou = sum(item["ious"]) / len(item["ious"]) if item["ious"] else 0.0
        print(f"class={class_id} name={name} tp={item['tp']} fp={item['fp']} fn={item['fn']} "
              f"precision={precision:.4f} recall={recall:.4f} f1={f1:.4f} mean_iou={mean_iou:.4f}")
    precision = total["tp"] / max(1, total["tp"] + total["fp"])
    recall = total["tp"] / max(1, total["tp"] + total["fn"])
    f1 = 2 * precision * recall / max(1e-12, precision + recall)
    mean_iou = sum(total["ious"]) / len(total["ious"]) if total["ious"] else 0.0
    print(f"overall tp={total['tp']} fp={total['fp']} fn={total['fn']} precision={precision:.4f} "
          f"recall={recall:.4f} f1={f1:.4f} mean_iou={mean_iou:.4f}")


if __name__ == "__main__":
    main()
