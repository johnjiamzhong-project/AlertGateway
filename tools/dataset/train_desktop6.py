#!/usr/bin/env python3
"""Train YOLOv8s on a desktop six-class dataset safely.

Training always uses the installed modern Ultralytics package. Rockchip export,
when requested, runs in a separate process using export_rockchip_onnx.py.
"""

import argparse
import subprocess
import sys
from pathlib import Path

DEFAULT_DATA = "/home/rambos/datasets/alertgateway_desktop6_final/data.yaml"
DEFAULT_WEIGHTS = "/home/rambos/yolov8_models/pytorch/yolov8s.pt"
DEFAULT_PROJECT = "/home/rambos/datasets/alertgateway_desktop6_final/runs"


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weights", default=DEFAULT_WEIGHTS)
    parser.add_argument("--data", default=DEFAULT_DATA)
    parser.add_argument("--epochs", type=int, default=50)
    parser.add_argument("--batch", type=int, default=8)
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--device", default="0")
    parser.add_argument("--project", default=DEFAULT_PROJECT)
    parser.add_argument("--name", default="yolov8s_desktop6_tune")
    parser.add_argument(
        "--ultralytics-dir",
        default=None,
        help="Deprecated and rejected; training must use installed modern Ultralytics",
    )
    parser.add_argument(
        "--export",
        action="store_true",
        help="Run the isolated Rockchip exporter after training",
    )
    parser.add_argument(
        "--export-python",
        default="/home/rambos/rknn-env/bin/python",
        help="Python executable for the isolated Rockchip export process",
    )
    parser.add_argument("--amp", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.ultralytics_dir:
        raise ValueError(
            "--ultralytics-dir is rejected for training; use installed modern Ultralytics"
        )

    from ultralytics import YOLO
    import ultralytics

    print("=" * 60)
    print("YOLOv8s Desktop6 Fine-Tuning")
    print("=" * 60)
    print(f"Ultralytics loaded from: {sys.modules['ultralytics'].__file__}")
    print(f"Ultralytics version:    {getattr(ultralytics, '__version__', 'unknown')}")
    print(f"Dataset:                {args.data}")
    print(f"Starting weights:       {args.weights}")
    print(
        f"Training parameters:    epochs={args.epochs}, batch={args.batch}, "
        f"imgsz={args.imgsz}, device={args.device}, amp={args.amp}"
    )
    print(f"Output directory:       {args.project}/{args.name}")
    print("=" * 60)

    model = YOLO(args.weights)
    model.train(
        data=args.data,
        epochs=args.epochs,
        batch=args.batch,
        imgsz=args.imgsz,
        device=args.device,
        project=args.project,
        name=args.name,
        workers=2,
        exist_ok=True,
        amp=args.amp,
    )
    print("\nTraining completed successfully!")

    best_weights = Path(args.project) / args.name / "weights" / "best.pt"
    if not best_weights.exists():
        raise FileNotFoundError(f"training completed without best.pt: {best_weights}")

    if args.export:
        print("\n--- Isolated Rockchip export ---")
        export_script = Path(__file__).with_name("export_rockchip_onnx.py")
        destination = Path(args.project) / args.name / f"{args.name}_native_640.onnx"
        subprocess.run(
            [
                args.export_python,
                str(export_script),
                "--weights",
                str(best_weights),
                "--output",
                str(destination),
                "--imgsz",
                str(args.imgsz),
            ],
            check=True,
        )
        print(f"Rockchip ONNX saved to {destination}")
    else:
        print("\nSkipping Rockchip export; pass --export to run it separately.")

    print("\nNext steps:")
    print("  1. Convert the ONNX with convert_int8.py.")
    print("  2. Evaluate the fixed final set before board deployment.")


if __name__ == "__main__":
    raise SystemExit(main())
