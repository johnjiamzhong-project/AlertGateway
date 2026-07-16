#!/usr/bin/env python3
"""Prepare a deterministic, session-isolated desktop6 evaluation review workspace."""

import argparse
import csv
import shutil
import time
from collections import Counter, defaultdict
from pathlib import Path


DEFAULT_ANNOTATION_DIR = Path("/home/rambos/datasets/alertgateway_desktop6/annotation")
DEFAULT_OUTPUT_DIR = Path("/home/rambos/datasets/alertgateway_desktop6/eval_review")

# Specialized sessions are combined so that both evaluation splits cover all six classes.
SELECTION_PLAN = (
    ("val", "collect_001_all_objects", 30),
    ("val", "collect_006_laptop_monitor_clean_2s", None),
    ("test", "collect_003_cup_book_phone", 30),
    ("test", "collect_005_laptop_monitor_2s", None),
)


def session_from_name(image_name: str) -> str:
    if "__" not in image_name:
        raise ValueError(f"image name does not contain a session prefix: {image_name}")
    return image_name.split("__", 1)[0]


def evenly_spaced(items: list[Path], count: int | None) -> list[Path]:
    if count is None or count >= len(items):
        return items
    if count <= 0:
        raise ValueError(f"sample count must be positive, got {count}")
    if count == 1:
        return [items[len(items) // 2]]
    indexes = [(i * (len(items) - 1)) // (count - 1) for i in range(count)]
    if len(set(indexes)) != count:
        raise RuntimeError("even sampling generated duplicate indexes")
    return [items[index] for index in indexes]


def read_negative_hints(manifest_path: Path) -> dict[str, str]:
    if not manifest_path.exists():
        return {}
    with manifest_path.open(newline="", encoding="utf-8") as handle:
        return {
            row["image_name"]: row.get("negative_hint", "")
            for row in csv.DictReader(handle)
        }


def validate_label(label_path: Path, class_counts: Counter[int]) -> int:
    box_count = 0
    for line_number, line in enumerate(label_path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        fields = line.split()
        if len(fields) != 5:
            raise ValueError(f"{label_path}:{line_number}: expected 5 fields")
        class_id = int(fields[0])
        x, y, width, height = map(float, fields[1:])
        if class_id not in range(6):
            raise ValueError(f"{label_path}:{line_number}: invalid class {class_id}")
        if not (0.0 <= x <= 1.0 and 0.0 <= y <= 1.0):
            raise ValueError(f"{label_path}:{line_number}: center outside [0, 1]")
        if not (0.0 < width <= 1.0 and 0.0 < height <= 1.0):
            raise ValueError(f"{label_path}:{line_number}: invalid box size")
        class_counts[class_id] += 1
        box_count += 1
    return box_count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--annotation-dir", type=Path, default=DEFAULT_ANNOTATION_DIR)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    source_images = args.annotation_dir / "images"
    source_labels = args.annotation_dir / "labels"
    classes_path = args.annotation_dir / "classes.txt"
    output_images = args.output_dir / "images"
    output_labels = args.output_dir / "labels"
    output_manifest = args.output_dir / "annotation_manifest.csv"
    prepared_marker = args.output_dir / ".prepared_at"

    for required in (source_images, source_labels, classes_path):
        if not required.exists():
            raise FileNotFoundError(required)

    if args.output_dir.exists() and any(args.output_dir.iterdir()):
        raise FileExistsError(
            f"output directory is not empty: {args.output_dir}; preserve or move it before rerunning"
        )

    by_session: dict[str, list[Path]] = defaultdict(list)
    for image_path in sorted(source_images.glob("*.jpg")):
        by_session[session_from_name(image_path.name)].append(image_path)

    negative_hints = read_negative_hints(args.annotation_dir / "annotation_manifest.csv")
    rows = []
    split_class_counts: dict[str, Counter[int]] = defaultdict(Counter)
    split_box_counts: Counter[str] = Counter()
    split_image_counts: Counter[str] = Counter()

    for target_split, session, sample_count in SELECTION_PLAN:
        candidates = by_session.get(session, [])
        if not candidates:
            raise ValueError(f"no active images found for session: {session}")
        selected = evenly_spaced(candidates, sample_count)
        for image_path in selected:
            label_path = source_labels / f"{image_path.stem}.txt"
            if not label_path.exists():
                raise FileNotFoundError(label_path)
            split_box_counts[target_split] += validate_label(
                label_path, split_class_counts[target_split]
            )
            split_image_counts[target_split] += 1
            rows.append(
                {
                    "image_name": image_path.name,
                    "session": session,
                    "target_split": target_split,
                    "source_image_path": str(image_path),
                    "source_label_path": str(label_path),
                    "negative_hint": negative_hints.get(image_path.name, ""),
                }
            )

    if not args.dry_run:
        output_images.mkdir(parents=True)
        output_labels.mkdir(parents=True)
        shutil.copy2(classes_path, args.output_dir / "classes.txt")
        for row in rows:
            shutil.copy2(row["source_image_path"], output_images / row["image_name"])
            shutil.copy2(
                row["source_label_path"],
                output_labels / f"{Path(row['image_name']).stem}.txt",
            )
        with output_manifest.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)
        prepared_marker.write_text(f"{time.time():.6f}\n", encoding="utf-8")

    print(f"annotation_dir: {args.annotation_dir}")
    print(f"output_dir: {args.output_dir}")
    print(f"dry_run: {args.dry_run}")
    for split in ("val", "test"):
        counts = split_class_counts[split]
        print(
            f"{split}: images={split_image_counts[split]}, boxes={split_box_counts[split]}, "
            f"classes={[counts[index] for index in range(6)]}"
        )
    if not args.dry_run:
        print(f"manifest: {output_manifest}")
        print(f"prepared_marker: {prepared_marker}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
