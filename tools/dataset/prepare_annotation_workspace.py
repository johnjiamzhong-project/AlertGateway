#!/usr/bin/env python3
import argparse
import csv
import shutil
from pathlib import Path


CLASSES = [
    "cell phone",
    "cup",
    "keyboard",
    "mouse",
    "laptop",
    "book",
]

DEFAULT_SELECTED_DIR = Path("/home/rambos/datasets/selected_desktop6/images")
DEFAULT_DATASET_ROOT = Path("/home/rambos/datasets/alertgateway_desktop6")
NEGATIVE_HINT_SESSIONS = {
    "collect_010_clutter_negative_2s",
    "collect_011_empty_negative_2s",
}


def session_from_name(image: Path) -> str:
    return image.name.split("__", 1)[0]


def iter_images(selected_dir: Path):
    return sorted(selected_dir.glob("*.jpg"))


def copy_image(source: Path, destination: Path, mode: str):
    if mode == "copy":
        shutil.copy2(source, destination)
    elif mode == "hardlink":
        destination.hardlink_to(source)
    elif mode == "symlink":
        destination.symlink_to(source)
    else:
        raise ValueError(f"unsupported mode: {mode}")


def write_classes(annotation_dir: Path):
    classes_path = annotation_dir / "classes.txt"
    classes_path.write_text("\n".join(CLASSES) + "\n", encoding="utf-8")
    return classes_path


def write_manifest(rows, manifest_path: Path):
    with manifest_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "image_name",
                "session",
                "source_path",
                "annotation_image_path",
                "label_path",
                "negative_hint",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Prepare an annotation workspace from selected desktop6 images."
    )
    parser.add_argument("--selected-dir", type=Path, default=DEFAULT_SELECTED_DIR)
    parser.add_argument("--dataset-root", type=Path, default=DEFAULT_DATASET_ROOT)
    parser.add_argument(
        "--mode",
        choices=["copy", "hardlink", "symlink"],
        default="copy",
        help="How to place images in annotation/images.",
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    images = list(iter_images(args.selected_dir))
    annotation_dir = args.dataset_root / "annotation"
    annotation_images_dir = annotation_dir / "images"
    annotation_labels_dir = annotation_dir / "labels"
    manifest_path = annotation_dir / "annotation_manifest.csv"

    copied = 0
    skipped_existing = 0
    rows = []

    if not args.dry_run:
        annotation_images_dir.mkdir(parents=True, exist_ok=True)
        annotation_labels_dir.mkdir(parents=True, exist_ok=True)
        write_classes(annotation_dir)

    for source in images:
        destination = annotation_images_dir / source.name
        label_path = annotation_labels_dir / f"{source.stem}.txt"
        session = session_from_name(source)
        rows.append(
            {
                "image_name": source.name,
                "session": session,
                "source_path": str(source),
                "annotation_image_path": str(destination),
                "label_path": str(label_path),
                "negative_hint": "1" if session in NEGATIVE_HINT_SESSIONS else "",
            }
        )

        if args.dry_run:
            copied += 1
            continue

        if destination.exists():
            skipped_existing += 1
            continue

        copy_image(source, destination, args.mode)
        copied += 1

    if not args.dry_run:
        write_manifest(rows, manifest_path)

    session_counts = {}
    for row in rows:
        session_counts[row["session"]] = session_counts.get(row["session"], 0) + 1

    print(f"selected_images: {len(images)}")
    print(f"annotation_dir: {annotation_dir}")
    print(f"mode: {args.mode}")
    print(f"{'would_place' if args.dry_run else 'placed'}: {copied}")
    print(f"skipped_existing: {skipped_existing}")
    print(f"negative_hint_images: {sum(1 for row in rows if row['negative_hint'])}")
    for session, count in sorted(session_counts.items()):
        print(f"session {session}: {count}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
