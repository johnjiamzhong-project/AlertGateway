#!/usr/bin/env python3
"""Export the native Rockchip-optimized YOLOv8s ONNX at 640x480."""

import argparse
import sys
from pathlib import Path


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
        "--output",
        default="/home/rambos/yolov8s_official_native_640x480.onnx",
        help="destination ONNX path",
    )
    parser.add_argument("--width", type=int, default=640, help="model input width")
    parser.add_argument("--height", type=int, default=480, help="model input height")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.width <= 0 or args.height <= 0:
        raise ValueError("--width and --height must be positive")

    sys.path.insert(0, args.ultralytics_dir)
    from ultralytics import YOLO

    print("Ultralytics package loaded from:", sys.modules["ultralytics"].__file__)
    model = YOLO(args.weights)

    output_path = Path(args.output).expanduser()
    default_export = Path(args.weights).expanduser().with_suffix(".onnx")

    print(f"Exporting native Rockchip model at {args.width}x{args.height}...")
    model.export(format="rknn", imgsz=[args.height, args.width])

    if default_export != output_path:
        if not default_export.exists():
            raise FileNotFoundError(f"expected export output not found: {default_export}")
        output_path.parent.mkdir(parents=True, exist_ok=True)
        default_export.replace(output_path)

    print(f"Saved ONNX to {output_path}")


if __name__ == "__main__":
    main()
