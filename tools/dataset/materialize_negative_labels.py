#!/usr/bin/env python3
import argparse
import csv
from pathlib import Path


DEFAULT_ANNOTATION_DIR = Path("/home/rambos/datasets/alertgateway_desktop6/annotation")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create empty YOLO label files for manifest rows marked as negative_hint=1."
    )
    parser.add_argument("--annotation-dir", type=Path, default=DEFAULT_ANNOTATION_DIR)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    annotation_dir = args.annotation_dir
    manifest_path = annotation_dir / "annotation_manifest.csv"
    labels_dir = annotation_dir / "labels"
    pending_path = annotation_dir / "pending_manual_review.csv"

    if not manifest_path.exists():
        raise FileNotFoundError(manifest_path)

    labels_dir.mkdir(parents=True, exist_ok=True)

    created = 0
    already_labeled = 0
    pending_rows = []

    with manifest_path.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            image_name = row["image_name"]
            label_path = labels_dir / f"{Path(image_name).stem}.txt"
            has_label = label_path.exists()
            if row.get("negative_hint") == "1":
                if has_label:
                    already_labeled += 1
                    continue
                if not args.dry_run:
                    label_path.write_text("", encoding="utf-8")
                created += 1
                continue
            if not has_label:
                pending_rows.append(row)

    if not args.dry_run:
        with pending_path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=["image_name", "session", "source_path", "annotation_image_path", "label_path", "negative_hint"])
            writer.writeheader()
            writer.writerows(pending_rows)

    print(f"negative_hint_rows: {created + already_labeled}")
    print(f"created_empty_labels: {created}")
    print(f"already_had_labels: {already_labeled}")
    print(f"pending_manual_review: {len(pending_rows)}")
    print(f"pending_review_csv: {pending_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
