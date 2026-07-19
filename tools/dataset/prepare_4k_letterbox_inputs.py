#!/usr/bin/env python3
"""Materialize exact 640x640 production-letterbox previews for 4K photos.

This does not replace the native-resolution annotation images.  It creates a
lossless visual copy of the pixel geometry used by InferThread's CPU fallback:
nearest-neighbour resize, centred padding and BGR/RGB value 114.  The manifest
records the forward transform needed to map a native-image annotation to model
coordinates.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import cv2
import numpy as np


def letterbox_like_infer_thread(image: np.ndarray, size: int) -> tuple[np.ndarray, dict[str, int | float]]:
    source_height, source_width = image.shape[:2]
    scale = min(size / source_width, size / source_height)
    resized_width = round(source_width * scale)
    resized_height = round(source_height * scale)
    left = (size - resized_width) // 2
    top = (size - resized_height) // 2

    # This matches resize_rgb_letterbox() in InferThread.cpp exactly:
    # source coordinate = destination coordinate * source / resized dimension.
    source_x = np.arange(resized_width, dtype=np.int64) * source_width // resized_width
    source_y = np.arange(resized_height, dtype=np.int64) * source_height // resized_height
    inner = image[source_y[:, None], source_x[None, :]]
    output = np.full((size, size, 3), 114, dtype=np.uint8)
    output[top : top + resized_height, left : left + resized_width] = inner
    return output, {
        "source_width": source_width,
        "source_height": source_height,
        "model_width": size,
        "model_height": size,
        "scale_x": resized_width / source_width,
        "scale_y": resized_height / source_height,
        "resized_width": resized_width,
        "resized_height": resized_height,
        "pad_left": left,
        "pad_top": top,
        "pad_right": size - resized_width - left,
        "pad_bottom": size - resized_height - top,
        "pad_value_bgr": 114,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("/home/rambos/datasets/alertgateway_4k_photo_20260718/preprocessed/landscape_16x9/images"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("/home/rambos/datasets/alertgateway_4k_photo_20260718/model_input_640_letterbox"),
    )
    parser.add_argument("--size", type=int, default=640)
    args = parser.parse_args()
    if args.size <= 0:
        raise ValueError("--size must be positive")
    source = args.source.resolve()
    output = args.output.resolve()
    if not source.is_dir():
        raise FileNotFoundError(source)
    if output.exists():
        raise FileExistsError(f"refusing to overwrite existing output: {output}")

    images = sorted(path for path in source.iterdir() if path.suffix.lower() in {".jpg", ".jpeg"})
    if not images:
        raise ValueError(f"no JPEG images found in {source}")
    image_output = output / "images"
    image_output.mkdir(parents=True)
    rows: list[dict[str, str | int | float]] = []
    for image_path in images:
        image = cv2.imread(str(image_path), cv2.IMREAD_COLOR | cv2.IMREAD_IGNORE_ORIENTATION)
        if image is None:
            raise ValueError(f"cannot decode {image_path}")
        letterboxed, transform = letterbox_like_infer_thread(image, args.size)
        output_path = image_output / f"{image_path.stem}.png"
        if not cv2.imwrite(str(output_path), letterboxed):
            raise RuntimeError(f"failed to write {output_path}")
        rows.append({
            "source_image": str(image_path),
            "model_input_image": str(output_path),
            **transform,
        })

    with (output / "letterbox_manifest.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    (output / "README.txt").write_text(
        "These are lossless 640x640 previews matching InferThread.cpp CPU letterbox geometry.\n"
        "Annotate native 4K images in preprocessed/landscape_16x9/images instead.\n"
        "Use letterbox_manifest.csv to transform native coordinates if model-space coordinates are needed.\n",
        encoding="utf-8",
    )
    print(f"source={source}")
    print(f"output={output}")
    print(f"images={len(rows)} size={args.size} format=png")
    print("geometry=InferThread.cpp nearest-neighbour + centred BGR(114,114,114) padding")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
