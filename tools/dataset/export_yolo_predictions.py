#!/usr/bin/env python3
"""Export Ultralytics YOLO predictions as normalized YOLO label files."""

import argparse
from pathlib import Path

from ultralytics import YOLO


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--images", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--conf", type=float, default=0.30)
    parser.add_argument("--iou", type=float, default=0.45)
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--device", default="0")
    args = parser.parse_args()

    if not 0.0 <= args.conf <= 1.0 or not 0.0 < args.iou <= 1.0:
        raise ValueError("invalid --conf or --iou")
    image_paths = sorted(args.images.glob("*.jpg"))
    if not image_paths:
        raise FileNotFoundError(f"no .jpg files in {args.images}")
    args.out.mkdir(parents=True, exist_ok=True)
    model = YOLO(args.model)
    written, detections = 0, 0
    results = model.predict(source=[str(path) for path in image_paths], conf=args.conf, iou=args.iou,
                            imgsz=args.imgsz, device=args.device, stream=True, verbose=False)
    for image_path, result in zip(image_paths, results):
        if result.boxes is None or len(result.boxes) == 0:
            continue
        # Ultralytics may materialize a list input as image0/image1/... internally.
        # Preserve the caller's original stem so predictions pair with GT labels.
        label_path = args.out / f"{image_path.stem}.txt"
        lines = []
        for cls, xywhn in zip(result.boxes.cls.tolist(), result.boxes.xywhn.tolist()):
            class_id = int(cls)
            cx, cy, width, height = xywhn
            lines.append(f"{class_id} {cx:.6f} {cy:.6f} {width:.6f} {height:.6f}")
        label_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        written += 1
        detections += len(lines)
    print(f"images={len(image_paths)} labeled_images={written} detections={detections} out={args.out}")


if __name__ == "__main__":
    main()
