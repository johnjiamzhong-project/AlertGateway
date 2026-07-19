#!/usr/bin/env python3
"""Create one annotation workspace containing all normalized 4K photos.

The workspace uses hard links to the normalized JPGs, so images are neither
copied nor moved.  Labels live only in the new workspace.
"""

from __future__ import annotations

import argparse
import csv
import os
from pathlib import Path


CLASSES = ("cell phone", "cup", "keyboard", "mouse", "laptop", "book")
GROUPS = ("landscape_16x9", "portrait_9x16")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("/home/rambos/datasets/alertgateway_4k_photo_20260718/preprocessed"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("/home/rambos/datasets/alertgateway_4k_photo_20260718/annotation_preprocessed"),
    )
    args = parser.parse_args()
    source = args.source.resolve()
    output = args.output.resolve()
    if output.exists():
        raise FileExistsError(f"refusing to overwrite existing output: {output}")

    rows: list[dict[str, str | int]] = []
    for group in GROUPS:
        image_dir = source / group / "images"
        if not image_dir.is_dir():
            raise FileNotFoundError(image_dir)
        for image_path in sorted(image_dir.glob("*.jpg")):
            rows.append(
                {
                    "image_name": image_path.name,
                    "source_path": str(image_path),
                    "aspect_group": group,
                    "width": 3840 if group == "landscape_16x9" else 2160,
                    "height": 2160 if group == "landscape_16x9" else 3840,
                }
            )
    names = [str(row["image_name"]) for row in rows]
    if len(names) != len(set(names)):
        raise ValueError("normalized image names are not unique")

    images_dir = output / "images"
    labels_dir = output / "labels"
    images_dir.mkdir(parents=True)
    labels_dir.mkdir()
    for row in rows:
        os.link(Path(str(row["source_path"])), images_dir / str(row["image_name"]))
    (output / "classes.txt").write_text("\n".join(CLASSES) + "\n", encoding="utf-8")
    with (output / "annotation_manifest.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (output / "README.txt").write_text(
        "images/ contains hard links to the EXIF-normalized photo files.\n"
        "Annotate every visible target object; labels/ belongs only to this workspace.\n",
        encoding="utf-8",
    )
    print(f"source={source}")
    print(f"output={output}")
    print(f"images={len(rows)} landscape={sum(row['aspect_group'] == 'landscape_16x9' for row in rows)} portrait={sum(row['aspect_group'] == 'portrait_9x16' for row in rows)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
