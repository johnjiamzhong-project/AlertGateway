#!/usr/bin/env python3
"""Fine-tune a graph-level YOLO candidate from compatible baseline weights."""

import argparse
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', required=True, help='Candidate model YAML')
    parser.add_argument('--weights', required=True, help='Baseline checkpoint for partial transfer')
    parser.add_argument('--data', default='/home/rambos/datasets/alertgateway_desktop6_final/data.yaml')
    parser.add_argument('--epochs', type=int, default=30)
    parser.add_argument('--batch', type=int, default=8)
    parser.add_argument('--imgsz', type=int, default=640)
    parser.add_argument('--device', default='0')
    parser.add_argument('--project', default='/home/rambos/datasets/alertgateway_desktop6_final/runs')
    parser.add_argument('--name', required=True)
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--amp', action='store_true')
    return parser.parse_args()


def main():
    args = parse_args()
    from ultralytics import YOLO

    if not Path(args.model).is_file():
        raise FileNotFoundError(args.model)
    if not Path(args.weights).is_file():
        raise FileNotFoundError(args.weights)

    model = YOLO(args.model)
    model.load(args.weights)
    model.train(
        data=args.data,
        epochs=args.epochs,
        batch=args.batch,
        imgsz=args.imgsz,
        device=args.device,
        workers=args.workers,
        amp=args.amp,
        project=args.project,
        name=args.name,
        exist_ok=True,
    )


if __name__ == '__main__':
    main()