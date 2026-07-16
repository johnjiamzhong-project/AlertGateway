#!/usr/bin/env python3
import argparse
import csv
import shutil
from pathlib import Path


DEFAULT_ANNOTATION_DIR = Path("/home/rambos/datasets/alertgateway_desktop6/annotation")
DEFAULT_OUTPUT_DIR = Path("/home/rambos/datasets/alertgateway_desktop6")
DEFAULT_SPLITS = {
    "train": {
        "collect_001_all_objects",
        "collect_002_keyboard_mouse_phone",
        "collect_003_cup_book_phone",
        "collect_004_keyboard_mouse_cup",
        "collect_005_laptop_monitor_2s",
        "collect_006_laptop_monitor_clean_2s",
        "collect_007_laptop_open_occluded_2s",
        "collect_008_cup_variants_2s",
    },
    "val": {"collect_009_book_variants_2s"},
    "test": {"collect_010_clutter_negative_2s", "collect_011_empty_negative_2s"},
}


def copy_item(source: Path, destination: Path, mode: str):
    if mode == "copy":
        shutil.copy2(source, destination)
    elif mode == "hardlink":
        destination.hardlink_to(source)
    elif mode == "symlink":
        destination.symlink_to(source)
    else:
        raise ValueError(f"unsupported mode: {mode}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Split the desktop6 annotation workspace into train/val/test by session."
    )
    parser.add_argument("--annotation-dir", type=Path, default=DEFAULT_ANNOTATION_DIR)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--mode", choices=["copy", "hardlink", "symlink"], default="hardlink")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    annotation_dir = args.annotation_dir
    output_dir = args.output_dir
    manifest_path = annotation_dir / "annotation_manifest.csv"
    labels_dir = annotation_dir / "labels"
    images_dir = annotation_dir / "images"
    pending_review_path = annotation_dir / "pending_manual_review.csv"

    if not manifest_path.exists():
        raise FileNotFoundError(manifest_path)

    pending = set()
    if pending_review_path.exists():
        with pending_review_path.open(newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                pending.add(row["image_name"])

    split_dirs = {
        split: (output_dir / split / "images", output_dir / split / "labels")
        for split in DEFAULT_SPLITS
    }
    if not args.dry_run:
        for img_dir, lbl_dir in split_dirs.values():
            img_dir.mkdir(parents=True, exist_ok=True)
            lbl_dir.mkdir(parents=True, exist_ok=True)

    rows = []
    counts = {split: 0 for split in DEFAULT_SPLITS}
    skipped_pending = 0

    with manifest_path.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            image_name = row["image_name"]
            if image_name in pending:
                skipped_pending += 1
                continue

            session = row["session"]
            split = None
            for candidate, sessions in DEFAULT_SPLITS.items():
                if session in sessions:
                    split = candidate
                    break
            if split is None:
                raise ValueError(f"unassigned session: {session}")

            image_source = images_dir / image_name
            label_source = labels_dir / f"{Path(image_name).stem}.txt"
            if not image_source.exists():
                raise FileNotFoundError(image_source)
            if not label_source.exists():
                raise FileNotFoundError(label_source)

            image_dest = split_dirs[split][0] / image_name
            label_dest = split_dirs[split][1] / label_source.name
            rows.append(
                {
                    "image_name": image_name,
                    "session": session,
                    "split": split,
                    "source_image_path": str(image_source),
                    "source_label_path": str(label_source),
                    "split_image_path": str(image_dest),
                    "split_label_path": str(label_dest),
                    "negative_hint": row.get("negative_hint", ""),
                }
            )

            if args.dry_run:
                counts[split] += 1
                continue

            copy_item(image_source, image_dest, args.mode)
            copy_item(label_source, label_dest, args.mode)
            counts[split] += 1

    split_manifest_path = output_dir / "split_manifest.csv"
    if not args.dry_run:
        with split_manifest_path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(
                f,
                fieldnames=[
                    "image_name",
                    "session",
                    "split",
                    "source_image_path",
                    "source_label_path",
                    "split_image_path",
                    "split_label_path",
                    "negative_hint",
                ],
            )
            writer.writeheader()
            writer.writerows(rows)

    print(f"annotation_dir: {annotation_dir}")
    print(f"output_dir: {output_dir}")
    print(f"mode: {args.mode}")
    for split in ("train", "val", "test"):
        print(f"{split}: {counts[split]}")
    print(f"skipped_pending_review: {skipped_pending}")
    print(f"split_manifest: {split_manifest_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
