#!/usr/bin/env python3
"""Build a deterministic RKNN calibration set from final desktop6 training images."""

import argparse
from pathlib import Path

import cv2


DEFAULT_IMAGES_DIR = Path("/home/rambos/datasets/alertgateway_desktop6_final/train/images")
DEFAULT_OUTPUT_DIR = Path("/home/rambos/datasets/alertgateway_desktop6_final/calibration_640")


def evenly_spaced(items: list[Path], count: int) -> list[Path]:
    if count >= len(items):
        return items
    indexes = [(index * (len(items) - 1)) // (count - 1) for index in range(count)]
    if len(set(indexes)) != count:
        raise RuntimeError("even sampling generated duplicate indexes")
    return [items[index] for index in indexes]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images-dir", type=Path, default=DEFAULT_IMAGES_DIR)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--count", type=int, default=150)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=640)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.count <= 1 or args.width <= 0 or args.height <= 0:
        raise ValueError("count must be greater than 1 and dimensions must be positive")
    images = sorted(args.images_dir.glob("*.jpg"))
    if len(images) < args.count:
        raise ValueError(f"requested {args.count} images, only {len(images)} are available")
    selected = evenly_spaced(images, args.count)

    if args.output_dir.exists() and any(args.output_dir.iterdir()):
        raise FileExistsError(f"output directory is not empty: {args.output_dir}")

    if not args.dry_run:
        args.output_dir.mkdir(parents=True)
        output_paths = []
        for index, image_path in enumerate(selected):
            image = cv2.imread(str(image_path), cv2.IMREAD_COLOR)
            if image is None:
                raise ValueError(f"failed to decode image: {image_path}")
            resized = cv2.resize(image, (args.width, args.height), interpolation=cv2.INTER_LINEAR)
            output_path = args.output_dir / f"calib_{index:04d}.png"
            if not cv2.imwrite(str(output_path), resized):
                raise RuntimeError(f"failed to write image: {output_path}")
            output_paths.append(output_path)
        (args.output_dir / "dataset.txt").write_text(
            "".join(f"{path}\n" for path in output_paths), encoding="utf-8"
        )

    print(f"images_dir: {args.images_dir}")
    print(f"output_dir: {args.output_dir}")
    print(f"selected: {len(selected)}")
    print(f"shape: {args.width}x{args.height}")
    print(f"dry_run: {args.dry_run}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
