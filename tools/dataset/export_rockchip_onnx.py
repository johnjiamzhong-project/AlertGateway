#!/usr/bin/env python3
"""Export trained weights with the custom Rockchip Ultralytics fork in isolation."""

import argparse
import sys
from pathlib import Path


DEFAULT_ULTRALYTICS_DIR = "/home/rambos/arm_test/ultralytics_yolov8"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weights", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--ultralytics-dir", default=DEFAULT_ULTRALYTICS_DIR)
    args = parser.parse_args()

    if not args.weights.exists():
        raise FileNotFoundError(args.weights)
    if args.output.exists():
        raise FileExistsError(args.output)

    sys.path.insert(0, args.ultralytics_dir)
    from ultralytics import YOLO

    print(f"Custom Ultralytics loaded from: {sys.modules['ultralytics'].__file__}")
    YOLO(args.weights).export(format="rknn", imgsz=[args.imgsz, args.imgsz])

    generated = args.weights.with_suffix(".onnx")
    if not generated.exists():
        raise FileNotFoundError(f"expected exported ONNX was not created: {generated}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    generated.replace(args.output)
    print(f"Rockchip ONNX saved to: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
