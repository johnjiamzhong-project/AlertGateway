#!/usr/bin/env python3
"""Normalize a new-keyboard photo set into an isolated challenge workspace."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import cv2

from normalize_4k_photo_images import apply_orientation, exif_orientation


CLASSES = ("cell phone", "cup", "keyboard", "mouse", "laptop", "book")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("/mnt/g/source/vscode/AlertGateway/test/keyboard2"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("/home/rambos/datasets/alertgateway_4k_photo_20260718/final_test_keyboard_challenge"),
    )
    parser.add_argument("--quality", type=int, default=95)
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    if not source.is_dir():
        raise FileNotFoundError(source)
    if output.exists():
        raise FileExistsError(f"refusing to overwrite existing output: {output}")
    images = sorted(path for path in source.iterdir() if path.suffix.lower() in {".jpg", ".jpeg"})
    if not images:
        raise ValueError(f"no JPEG images in {source}")

    images_dir = output / "images"
    labels_dir = output / "labels"
    images_dir.mkdir(parents=True)
    labels_dir.mkdir()
    rows = []
    for source_path in images:
        image = cv2.imread(str(source_path), cv2.IMREAD_COLOR | cv2.IMREAD_IGNORE_ORIENTATION)
        if image is None:
            raise ValueError(f"cannot decode {source_path}")
        orientation = exif_orientation(source_path)
        normalized = apply_orientation(image, orientation)
        height, width = normalized.shape[:2]
        image_name = f"2_keyboard2__{source_path.name}"
        target = images_dir / image_name
        if not cv2.imwrite(str(target), normalized, [cv2.IMWRITE_JPEG_QUALITY, args.quality]):
            raise RuntimeError(f"failed to write {target}")
        rows.append(
            {
                "image_name": image_name,
                "source_path": str(source_path),
                "exif_orientation": orientation,
                "width": width,
                "height": height,
            }
        )
    (output / "classes.txt").write_text("\n".join(CLASSES) + "\n", encoding="utf-8")
    with (output / "annotation_manifest.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (output / "README.txt").write_text(
        "Isolated green/white keyboard challenge set. Do not use for train or val.\n"
        "After manual annotation, evaluate a finalized 4K candidate once without tuning on this set.\n",
        encoding="utf-8",
    )
    names = ", ".join(f"{index}: {name}" for index, name in enumerate(CLASSES))
    (output / "data.yaml").write_text(
        f"path: {output}\n"
        "train: images\n"
        "val: images\n"
        "test: images\n"
        f"names: {{{names}}}\n",
        encoding="utf-8",
    )
    print(f"source={source}\noutput={output}\nimages={len(rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
