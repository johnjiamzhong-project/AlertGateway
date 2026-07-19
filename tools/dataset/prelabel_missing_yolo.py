#!/usr/bin/env python3
"""Create YOLO draft labels only for images that have no label file yet."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from ultralytics import YOLO


EXPECTED_CLASSES = ("cell phone", "cup", "keyboard", "mouse", "laptop", "book")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--annotation-dir", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--conf", type=float, default=0.30)
    parser.add_argument("--iou", type=float, default=0.45)
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--device", default="0")
    parser.add_argument("--batch", type=int, default=16)
    args = parser.parse_args()
    if not 0.0 <= args.conf <= 1.0 or not 0.0 < args.iou <= 1.0 or args.batch <= 0:
        raise ValueError("invalid --conf or --iou")

    annotation_dir = args.annotation_dir.resolve()
    images_dir = annotation_dir / "images"
    labels_dir = annotation_dir / "labels"
    if not images_dir.is_dir() or not labels_dir.is_dir():
        raise FileNotFoundError("annotation directory needs images/ and labels/")
    images = sorted(images_dir.glob("*.jpg"))
    pending = [image for image in images if not (labels_dir / f"{image.stem}.txt").exists()]
    print(f"images={len(images)} existing_before={len(images) - len(pending)} pending={len(pending)}", flush=True)
    model = YOLO(args.model)
    names = tuple(str(model.names[index]) for index in range(len(model.names)))
    if names != EXPECTED_CLASSES:
        raise ValueError(f"model classes mismatch: {names!r}, expected {EXPECTED_CLASSES!r}")
    print(f"model_classes={','.join(names)} batch={args.batch} device={args.device}", flush=True)

    rows: list[dict[str, str | int]] = []
    for start in range(0, len(pending), args.batch):
        batch = pending[start : start + args.batch]
        results = model.predict(
            source=[str(image) for image in batch],
            conf=args.conf,
            iou=args.iou,
            imgsz=args.imgsz,
            device=args.device,
            stream=False,
            verbose=False,
        )
        for image, result in zip(batch, results):
            label_path = labels_dir / f"{image.stem}.txt"
            # The annotator can save while prediction is running.  Never replace
            # a user label that appeared after the pending list was created.
            if label_path.exists():
                rows.append({"image_name": image.name, "status": "skipped_saved_during_run", "boxes": 0})
                continue
            lines = []
            if result.boxes is not None:
                for class_id, xywhn in zip(result.boxes.cls.tolist(), result.boxes.xywhn.tolist()):
                    cx, cy, width, height = xywhn
                    lines.append(f"{int(class_id)} {cx:.6f} {cy:.6f} {width:.6f} {height:.6f}")
            # An empty file is an explicit model draft, not an asserted negative.
            label_path.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")
            rows.append({"image_name": image.name, "status": "draft" if lines else "draft_empty", "boxes": len(lines)})
        print(f"processed={min(start + len(batch), len(pending))}/{len(pending)}", flush=True)

    manifest = annotation_dir / "pseudo_label_manifest_4k_photos.csv"
    with manifest.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=("image_name", "status", "boxes"))
        writer.writeheader()
        writer.writerows(rows)
    print(f"drafts={sum(row['status'] == 'draft' for row in rows)} draft_empty={sum(row['status'] == 'draft_empty' for row in rows)} skipped_saved_during_run={sum(row['status'] == 'skipped_saved_during_run' for row in rows)}")
    print(f"boxes={sum(int(row['boxes']) for row in rows)} manifest={manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
