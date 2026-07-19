#!/usr/bin/env python3
"""Normalize EXIF orientation in the 4K photo collection.

The source files are never changed. JPEG pixels are decoded, EXIF orientation
is applied, metadata is removed, and the result is re-encoded at quality 95.
Landscape and portrait images are kept in separate workspaces instead of
cropping one aspect ratio into the other.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
from pathlib import Path

import cv2


CLASSES = ("cell phone", "cup", "keyboard", "mouse", "laptop", "book")
SUFFIXES = {".jpg", ".jpeg"}


def exif_orientation(path: Path) -> int:
    data = path.read_bytes()
    offset = 2
    while offset + 4 <= len(data) and data[offset] == 0xFF:
        marker = data[offset + 1]
        offset += 2
        if marker in (0xD8, 0xD9):
            continue
        length = int.from_bytes(data[offset : offset + 2], "big")
        if marker == 0xE1 and data[offset + 2 : offset + 8] == b"Exif\x00\x00":
            tiff = offset + 8
            endian = data[tiff : tiff + 2]
            order = "little" if endian == b"II" else "big"

            def number(pos: int, size: int) -> int:
                return int.from_bytes(data[tiff + pos : tiff + pos + size], order)

            if number(2, 2) != 42:
                return 1
            ifd = number(4, 4)
            count = number(ifd, 2)
            for index in range(count):
                entry = ifd + 2 + index * 12
                if number(entry, 2) == 0x0112:
                    return number(entry + 8, 2)
            return 1
        offset += max(0, length - 2)
    return 1


def apply_orientation(image, orientation: int):
    if orientation == 2:
        return cv2.flip(image, 1)
    if orientation == 3:
        return cv2.rotate(image, cv2.ROTATE_180)
    if orientation == 4:
        return cv2.flip(image, 0)
    if orientation == 5:
        return cv2.transpose(image)
    if orientation == 6:
        return cv2.rotate(image, cv2.ROTATE_90_CLOCKWISE)
    if orientation == 7:
        return cv2.flip(cv2.transpose(image), -1)
    if orientation == 8:
        return cv2.rotate(image, cv2.ROTATE_90_COUNTERCLOCKWISE)
    return image


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("/mnt/g/source/vscode/AlertGateway"))
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("/home/rambos/datasets/alertgateway_4k_photo_20260718/preprocessed"),
    )
    parser.add_argument("--quality", type=int, default=95)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.quality <= 100:
        raise ValueError("--quality must be in [1, 100]")

    source = args.source.resolve()
    output = args.output.resolve()
    if not source.is_dir():
        raise FileNotFoundError(source)
    if output.exists() and not args.dry_run:
        raise FileExistsError(f"refusing to overwrite existing output: {output}")

    rows: list[dict[str, str | int]] = []
    for class_id, class_name in enumerate(CLASSES):
        class_dir = source / class_name
        for image_path in sorted(class_dir.iterdir()):
            if not image_path.is_file() or image_path.suffix.lower() not in SUFFIXES:
                continue
            # OpenCV applies EXIF orientation by default. Ignore it here so
            # that the explicit transform below is applied exactly once.
            image = cv2.imread(
                str(image_path), cv2.IMREAD_COLOR | cv2.IMREAD_IGNORE_ORIENTATION
            )
            if image is None:
                raise ValueError(f"cannot decode {image_path}")
            orientation = exif_orientation(image_path)
            normalized = apply_orientation(image, orientation)
            height, width = normalized.shape[:2]
            aspect = "landscape_16x9" if width > height else "portrait_9x16" if height > width else "other"
            output_name = f"{class_id}_{class_name.replace(' ', '_')}__{image_path.name}"
            row = {
                "source_class_id": class_id,
                "source_class": class_name,
                "source_path": str(image_path),
                "source_sha256": sha256(image_path),
                "source_width": image.shape[1],
                "source_height": image.shape[0],
                "exif_orientation": orientation,
                "normalized_name": output_name,
                "normalized_width": width,
                "normalized_height": height,
                "aspect_group": aspect,
            }
            rows.append(row)
            if args.dry_run:
                continue
            group = output / aspect
            image_dir = group / "images"
            label_dir = group / "labels"
            image_dir.mkdir(parents=True, exist_ok=True)
            label_dir.mkdir(parents=True, exist_ok=True)
            if not cv2.imwrite(
                str(image_dir / output_name),
                normalized,
                [cv2.IMWRITE_JPEG_QUALITY, args.quality],
            ):
                raise RuntimeError(f"failed to write {image_dir / output_name}")

    if not args.dry_run:
        for group in sorted({str(row["aspect_group"]) for row in rows}):
            group_dir = output / group
            (group_dir / "classes.txt").write_text("\n".join(CLASSES) + "\n", encoding="utf-8")
            group_rows = [row for row in rows if row["aspect_group"] == group]
            with (group_dir / "photo_manifest.csv").open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=list(group_rows[0]))
                writer.writeheader()
                writer.writerows(group_rows)

    print(f"source={source}")
    print(f"output={output}")
    print(f"images={len(rows)} mode={'dry-run' if args.dry_run else 'normalized'} quality={args.quality}")
    for group in ("landscape_16x9", "portrait_9x16", "other"):
        group_rows = [row for row in rows if row["aspect_group"] == group]
        if group_rows:
            print(f"group={group} images={len(group_rows)}")
            for class_name in CLASSES:
                count = sum(1 for row in group_rows if row["source_class"] == class_name)
                if count:
                    print(f"  class={class_name} images={count}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
