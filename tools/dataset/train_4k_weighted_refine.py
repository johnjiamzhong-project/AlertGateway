#!/usr/bin/env python3
"""Fine-tune the accepted desktop-six baseline on a weighted 4K mix."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--weights",
        default=(
            "/home/rambos/datasets/alertgateway_desktop6_final/runs/"
            "yolov8s_desktop6_final_30e_20260710/weights/best.pt"
        ),
    )
    parser.add_argument(
        "--data",
        default=(
            "/home/rambos/datasets/alertgateway_4k_refine/"
            "weighted_candidate_20260717/data.yaml"
        ),
    )
    parser.add_argument("--epochs", type=int, default=25)
    parser.add_argument("--batch", type=int, default=4)
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--device", default="0")
    parser.add_argument(
        "--project",
        default="/home/rambos/datasets/alertgateway_4k_refine/runs",
    )
    parser.add_argument("--name", default="yolov8s_4k_weighted_25e")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--freeze", type=int, default=None)
    parser.add_argument("--lr0", type=float, default=0.001)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    weights = Path(args.weights)
    data = Path(args.data)
    if not weights.is_file():
        raise FileNotFoundError(weights)
    if not data.is_file():
        raise FileNotFoundError(data)

    from ultralytics import YOLO
    import ultralytics

    print(f"ultralytics={getattr(ultralytics, '__version__', 'unknown')}")
    print(f"ultralytics_path={sys.modules['ultralytics'].__file__}")
    print(f"weights={weights}")
    print(f"data={data}")
    print(
        f"policy=AdamW lr0={args.lr0} cos_lr=True mosaic=0.5 "
        f"scale=0.35 freeze={args.freeze} AMP=False"
    )

    model = YOLO(str(weights))
    model.train(
        data=str(data),
        epochs=args.epochs,
        patience=8,
        batch=args.batch,
        imgsz=args.imgsz,
        device=args.device,
        workers=args.workers,
        project=args.project,
        name=args.name,
        exist_ok=False,
        optimizer="AdamW",
        lr0=args.lr0,
        lrf=0.05,
        cos_lr=True,
        warmup_epochs=1.0,
        weight_decay=0.0005,
        mosaic=0.5,
        close_mosaic=5,
        scale=0.35,
        translate=0.1,
        fliplr=0.5,
        erasing=0.2,
        amp=False,
        seed=0,
        deterministic=True,
        freeze=args.freeze,
        plots=True,
    )

    best = Path(args.project) / args.name / "weights" / "best.pt"
    if not best.is_file():
        raise FileNotFoundError(f"training finished without best checkpoint: {best}")
    print(f"best={best}")
    print("No ONNX/RKNN export was performed; evaluate both development domains first.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
