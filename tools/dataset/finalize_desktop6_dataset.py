#!/usr/bin/env python3
"""Build the final session-isolated desktop6 train/val/test dataset after review."""

import argparse
import csv
import os
from collections import Counter
from pathlib import Path


DEFAULT_ANNOTATION_DIR = Path("/home/rambos/datasets/alertgateway_desktop6/annotation")
DEFAULT_EVAL_REVIEW_DIR = Path("/home/rambos/datasets/alertgateway_desktop6/eval_review")
DEFAULT_OUTPUT_DIR = Path("/home/rambos/datasets/alertgateway_desktop6_final")

TRAIN_SESSIONS = {
    "collect_002_keyboard_mouse_phone",
    "collect_004_keyboard_mouse_cup",
    "collect_007_laptop_open_occluded_2s",
    "collect_008_cup_variants_2s",
    "collect_009_book_variants_2s",
    "collect_010_clutter_negative_2s",
    "collect_011_empty_negative_2s",
}

# Counts refer to the alphabetical candidates frozen in eval_review/annotation_manifest.csv.
EVAL_SELECTION = (
    ("val", "collect_001_all_objects", 20),
    ("test", "collect_003_cup_book_phone", 20),
    ("test", "collect_005_laptop_monitor_2s", 10),
    ("val", "collect_006_laptop_monitor_clean_2s", 10),
)

CLASS_NAMES = ("cell phone", "cup", "keyboard", "mouse", "laptop", "book")


def session_from_name(image_name: str) -> str:
    if "__" not in image_name:
        raise ValueError(f"image name does not contain a session prefix: {image_name}")
    return image_name.split("__", 1)[0]


def validate_label(label_path: Path, counts: Counter[int]) -> int:
    boxes = 0
    for line_number, line in enumerate(label_path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) != 5:
            raise ValueError(f"{label_path}:{line_number}: expected 5 fields")
        class_id = int(fields[0])
        x, y, width, height = map(float, fields[1:])
        if class_id not in range(len(CLASS_NAMES)):
            raise ValueError(f"{label_path}:{line_number}: invalid class {class_id}")
        if not (0.0 <= x <= 1.0 and 0.0 <= y <= 1.0):
            raise ValueError(f"{label_path}:{line_number}: center outside [0, 1]")
        if not (0.0 < width <= 1.0 and 0.0 < height <= 1.0):
            raise ValueError(f"{label_path}:{line_number}: invalid box size")
        counts[class_id] += 1
        boxes += 1
    return boxes


def link_file(source: Path, destination: Path) -> None:
    destination.hardlink_to(source)


def add_item(
    rows: list[dict[str, str]],
    split: str,
    session: str,
    image_source: Path,
    label_source: Path,
    output_dir: Path,
    dry_run: bool,
) -> None:
    image_destination = output_dir / split / "images" / image_source.name
    label_destination = output_dir / split / "labels" / label_source.name
    if not dry_run:
        link_file(image_source, image_destination)
        link_file(label_source, label_destination)
    rows.append(
        {
            "image_name": image_source.name,
            "session": session,
            "split": split,
            "source_image_path": str(image_source),
            "source_label_path": str(label_source),
            "split_image_path": str(image_destination),
            "split_label_path": str(label_destination),
        }
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--annotation-dir", type=Path, default=DEFAULT_ANNOTATION_DIR)
    parser.add_argument("--eval-review-dir", type=Path, default=DEFAULT_EVAL_REVIEW_DIR)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.output_dir.exists() and any(args.output_dir.iterdir()):
        raise FileExistsError(
            f"output directory is not empty: {args.output_dir}; preserve or move it before rerunning"
        )

    annotation_images = args.annotation_dir / "images"
    annotation_labels = args.annotation_dir / "labels"
    eval_images = args.eval_review_dir / "images"
    eval_labels = args.eval_review_dir / "labels"
    eval_manifest_path = args.eval_review_dir / "annotation_manifest.csv"
    for required in (
        annotation_images,
        annotation_labels,
        eval_images,
        eval_labels,
        eval_manifest_path,
    ):
        if not required.exists():
            raise FileNotFoundError(required)

    if not args.dry_run:
        for split in ("train", "val", "test"):
            (args.output_dir / split / "images").mkdir(parents=True)
            (args.output_dir / split / "labels").mkdir(parents=True)

    rows: list[dict[str, str]] = []
    split_counts = Counter()
    split_boxes = Counter()
    class_counts = {split: Counter() for split in ("train", "val", "test")}

    for image_path in sorted(annotation_images.glob("*.jpg")):
        session = session_from_name(image_path.name)
        if session not in TRAIN_SESSIONS:
            continue
        label_path = annotation_labels / f"{image_path.stem}.txt"
        if not label_path.exists():
            raise FileNotFoundError(label_path)
        split_boxes["train"] += validate_label(label_path, class_counts["train"])
        add_item(rows, "train", session, image_path, label_path, args.output_dir, args.dry_run)
        split_counts["train"] += 1

    with eval_manifest_path.open(newline="", encoding="utf-8") as handle:
        eval_rows = list(csv.DictReader(handle))

    for split, session, count in EVAL_SELECTION:
        candidates = sorted(
            (row for row in eval_rows if row["session"] == session),
            key=lambda row: row["image_name"],
        )[:count]
        if len(candidates) != count:
            raise ValueError(f"{session}: expected {count} frozen candidates, got {len(candidates)}")
        for candidate in candidates:
            image_path = eval_images / candidate["image_name"]
            label_path = eval_labels / f"{Path(candidate['image_name']).stem}.txt"
            # Deletions made during manual review are intentionally excluded.
            if not image_path.exists():
                continue
            if not label_path.exists():
                raise FileNotFoundError(label_path)
            split_boxes[split] += validate_label(label_path, class_counts[split])
            add_item(rows, split, session, image_path, label_path, args.output_dir, args.dry_run)
            split_counts[split] += 1

    split_sessions = {
        split: {row["session"] for row in rows if row["split"] == split}
        for split in ("train", "val", "test")
    }
    for left, right in (("train", "val"), ("train", "test"), ("val", "test")):
        overlap = split_sessions[left] & split_sessions[right]
        if overlap:
            raise ValueError(f"session leakage between {left} and {right}: {sorted(overlap)}")

    for split in ("val", "test"):
        missing = [index for index in range(len(CLASS_NAMES)) if class_counts[split][index] == 0]
        if missing:
            raise ValueError(f"{split} is missing classes: {missing}")

    if not args.dry_run:
        manifest_path = args.output_dir / "split_manifest.csv"
        with manifest_path.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)
        data_yaml = args.output_dir / "data.yaml"
        names = "\n".join(f"  {index}: {name}" for index, name in enumerate(CLASS_NAMES))
        data_yaml.write_text(
            f"path: {args.output_dir}\n"
            "train: train/images\n"
            "val: val/images\n"
            "test: test/images\n\n"
            f"names:\n{names}\n",
            encoding="utf-8",
        )

    print(f"annotation_dir: {args.annotation_dir}")
    print(f"eval_review_dir: {args.eval_review_dir}")
    print(f"output_dir: {args.output_dir}")
    print(f"dry_run: {args.dry_run}")
    for split in ("train", "val", "test"):
        print(
            f"{split}: images={split_counts[split]}, boxes={split_boxes[split]}, "
            f"classes={[class_counts[split][index] for index in range(len(CLASS_NAMES))]}, "
            f"sessions={sorted(split_sessions[split])}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
