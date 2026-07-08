#!/usr/bin/env python3
"""Export native Rockchip-optimized YOLOv8s ONNX candidates."""

import argparse
import sys
from pathlib import Path


DEFAULT_SIZES = ("576x448", "512x384")


def parse_size(value):
    try:
        width_text, height_text = value.lower().split("x", 1)
        width = int(width_text)
        height = int(height_text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("size must use WIDTHxHEIGHT, for example 576x448") from exc
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("width and height must be positive")
    return width, height


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--ultralytics-dir",
        default="/home/rambos/arm_test/ultralytics_yolov8",
        help="custom ultralytics_yolov8 checkout containing the Rockchip export path",
    )
    parser.add_argument(
        "--weights",
        default="/home/rambos/yolov8s.pt",
        help="source YOLOv8s .pt weights",
    )
    parser.add_argument(
        "--output-dir",
        default="/home/rambos",
        help="directory for exported ONNX files",
    )
    parser.add_argument(
        "--size",
        action="append",
        type=parse_size,
        default=None,
        help="candidate model size as WIDTHxHEIGHT; may be specified more than once",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    sys.path.insert(0, args.ultralytics_dir)
    from ultralytics import YOLO

    print("Ultralytics package loaded from:", sys.modules["ultralytics"].__file__)
    model = YOLO(args.weights)

    output_dir = Path(args.output_dir).expanduser()
    default_export = Path(args.weights).expanduser().with_suffix(".onnx")

    sizes = args.size or [parse_size(size) for size in DEFAULT_SIZES]
    for width, height in sizes:
        output_path = output_dir / f"yolov8s_official_native_{width}x{height}.onnx"
        print(f"\n>>> Exporting {width}x{height} model...")
        model.export(format="rknn", imgsz=[height, width])

        if not default_export.exists():
            raise FileNotFoundError(f"expected export output not found: {default_export}")
        output_dir.mkdir(parents=True, exist_ok=True)
        default_export.replace(output_path)
        print(f"Saved {width}x{height} model to {output_path}")

    print("All exports finished.")


if __name__ == "__main__":
    main()
