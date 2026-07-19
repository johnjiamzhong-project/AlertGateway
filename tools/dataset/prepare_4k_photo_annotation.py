#!/usr/bin/env python3
"""Prepare a 4K photo annotation workspace from per-class JPG folders.

The source tree is never moved or modified. Images are copied into a flat
annotation directory with collision-free names, while the original class
folder is recorded as a review hint in the manifest. Annotators must label
all visible target classes in each image, not only the source folder class.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import re
import shutil
from datetime import datetime
from pathlib import Path


CLASSES = ("cell phone", "cup", "keyboard", "mouse", "laptop", "book")
IMAGE_SUFFIXES = {".jpg", ".jpeg"}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def capture_time(name: str) -> str:
    match = re.search(r"IMG_(\d{8})_(\d{6})", name)
    if not match:
        return ""
    return datetime.strptime("_".join(match.groups()), "%Y%m%d_%H%M%S").isoformat(" ")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("/mnt/g/source/vscode/AlertGateway"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("/home/rambos/datasets/alertgateway_4k_photo_20260718"),
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    source = args.source.resolve()
    output = args.output.resolve()
    if not source.is_dir():
        raise FileNotFoundError(source)
    if output.exists() and not args.dry_run:
        raise FileExistsError(f"refusing to overwrite existing output: {output}")

    rows: list[dict[str, str | int]] = []
    seen_hashes: dict[str, str] = {}
    for class_id, class_name in enumerate(CLASSES):
        class_dir = source / class_name
        if not class_dir.is_dir():
            raise FileNotFoundError(class_dir)
        for image in sorted(class_dir.iterdir()):
            if not image.is_file() or image.suffix.lower() not in IMAGE_SUFFIXES:
                continue
            digest = sha256(image)
            duplicate_of = seen_hashes.get(digest, "")
            seen_hashes.setdefault(digest, str(image))
            output_name = f"{class_id}_{class_name.replace(' ', '_')}__{image.name}"
            rows.append(
                {
                    "source_class_id": class_id,
                    "source_class": class_name,
                    "source_name": image.name,
                    "source_path": str(image),
                    "annotation_name": output_name,
                    "sha256": digest,
                    "bytes": image.stat().st_size,
                    "capture_time": capture_time(image.name),
                    "duplicate_of": duplicate_of,
                    "label_status": "missing",
                }
            )

    if not args.dry_run:
        image_dir = output / "annotation" / "images"
        label_dir = output / "annotation" / "labels"
        image_dir.mkdir(parents=True)
        label_dir.mkdir(parents=True)
        for row in rows:
            source_path = Path(str(row["source_path"]))
            shutil.copy2(source_path, image_dir / str(row["annotation_name"]))
        (output / "annotation" / "classes.txt").write_text(
            "\n".join(CLASSES) + "\n", encoding="utf-8"
        )
        with (output / "annotation" / "photo_manifest.csv").open(
            "w", newline="", encoding="utf-8"
        ) as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        (output / "README.txt").write_text(
            "原始照片位于 Windows 目录，未移动或修改。\n"
            "annotation/images 是人工标注工作副本；annotation/labels 初始为空。\n"
            "source_class 只表示原始文件夹提示，标注时必须标出画面中所有可见目标。\n",
            encoding="utf-8",
        )

    duplicates = sum(1 for row in rows if row["duplicate_of"])
    print(f"source={source}")
    print(f"output={output}")
    print(f"images={len(rows)} unique={len(rows) - duplicates} duplicates={duplicates}")
    for class_name in CLASSES:
        count = sum(1 for row in rows if row["source_class"] == class_name)
        print(f"class={class_name} images={count}")
    print(f"mode={'dry-run' if args.dry_run else 'copy'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
