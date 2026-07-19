#!/usr/bin/env python3
"""Freeze verified 4K photo annotations into train/val directories.

Images are hard-linked to avoid duplication. Labels are copied so later edits
in an annotation workspace cannot silently alter a frozen split.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import os
import shutil
from pathlib import Path


CLASSES = ("cell phone", "cup", "keyboard", "mouse", "laptop", "book")


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def gather(annotation_dir: Path) -> list[tuple[Path, Path]]:
    images_dir = annotation_dir / "images"
    labels_dir = annotation_dir / "labels"
    pairs = []
    for image in sorted(images_dir.glob("*.jpg")):
        label = labels_dir / f"{image.stem}.txt"
        if not label.is_file():
            raise FileNotFoundError(f"missing label for {image.name}")
        pairs.append((image, label))
    if not pairs:
        raise ValueError(f"no images in {images_dir}")
    return pairs


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--train-annotation",
        type=Path,
        default=Path("/home/rambos/datasets/alertgateway_4k_photo_20260718/annotation_preprocessed"),
    )
    parser.add_argument(
        "--val-annotation",
        type=Path,
        default=Path("/home/rambos/datasets/alertgateway_4k_photo_20260718/validation_annotation"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("/home/rambos/datasets/alertgateway_4k_photo_20260718/train_val_frozen"),
    )
    args = parser.parse_args()
    train_annotation = args.train_annotation.resolve()
    val_annotation = args.val_annotation.resolve()
    output = args.output.resolve()
    if output.exists():
        raise FileExistsError(f"refusing to overwrite existing output: {output}")
    train_pairs = gather(train_annotation)
    val_pairs = gather(val_annotation)
    train_hashes = {digest(image) for image, _ in train_pairs}
    val_hashes = {digest(image) for image, _ in val_pairs}
    overlap = train_hashes & val_hashes
    if overlap:
        raise ValueError(f"train/val image hash overlap: {len(overlap)}")

    rows: list[dict[str, str]] = []
    for split, pairs in (("train", train_pairs), ("val", val_pairs)):
        image_dest = output / split / "images"
        label_dest = output / split / "labels"
        image_dest.mkdir(parents=True)
        label_dest.mkdir(parents=True)
        for image, label in pairs:
            os.link(image, image_dest / image.name)
            shutil.copy2(label, label_dest / label.name)
            rows.append(
                {
                    "split": split,
                    "image_name": image.name,
                    "source_image": str(image),
                    "source_label": str(label),
                    "image_sha256": digest(image),
                }
            )
    (output / "classes.txt").write_text("\n".join(CLASSES) + "\n", encoding="utf-8")
    (output / "data.yaml").write_text(
        "path: " + str(output) + "\n"
        "train: train/images\n"
        "val: val/images\n"
        "names:\n" + "".join(f"  {index}: {name}\n" for index, name in enumerate(CLASSES)),
        encoding="utf-8",
    )
    with (output / "split_manifest.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (output / "README.txt").write_text(
        "Frozen train/val split. Images are hard links; labels are copies.\n"
        "Do not use validation images for training or hyperparameter selection beyond model selection.\n",
        encoding="utf-8",
    )
    print(f"output={output}")
    print(f"train_images={len(train_pairs)} val_images={len(val_pairs)} hash_overlap={len(overlap)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
