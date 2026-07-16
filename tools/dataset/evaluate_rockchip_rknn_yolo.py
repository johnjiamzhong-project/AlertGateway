#!/usr/bin/env python3
"""Quantize a nine-output ONNX and evaluate it in RKNN Simulator on a YOLO split."""

from __future__ import annotations

import argparse
from pathlib import Path

import cv2
import numpy as np
from rknn.api import RKNN

from evaluate_rockchip_onnx_yolo import (
    CLASS_NAMES,
    IMAGE_SUFFIXES,
    decode,
    load_ground_truth,
    metrics,
    preprocess,
)


def evaluate(rknn: RKNN, dataset: Path, mode: str, args: argparse.Namespace) -> None:
    image_dir = dataset / args.split / "images"
    label_dir = dataset / args.split / "labels"
    images = sorted(path for path in image_dir.iterdir() if path.suffix.lower() in IMAGE_SUFFIXES)
    ground_truth: dict[str, list[dict]] = {}
    predictions: dict[str, list[dict]] = {}

    for image_path in images:
        image = cv2.imread(str(image_path))
        if image is None:
            raise ValueError(f"failed to read {image_path}")
        height, width = image.shape[:2]
        tensor, transform = preprocess(image, args.imgsz, mode)
        rgb = np.transpose(tensor[0], (1, 2, 0)) * 255.0
        outputs = rknn.inference(inputs=[rgb[None, ...].astype(np.uint8)])
        predictions[image_path.name] = decode(
            outputs, transform, width, height, args.conf_threshold, args.nms_threshold
        )
        ground_truth[image_path.name] = load_ground_truth(label_dir / f"{image_path.stem}.txt", width, height)

    per_class = metrics(ground_truth, predictions, args.eval_iou_threshold)
    mean_ap = float(np.mean([item["ap50"] for item in per_class]))
    empty_images = sum(not items for items in predictions.values())
    print(f"\n[RKNN INT8 {mode}] images={len(images)} predictions={sum(map(len, predictions.values()))} empty={empty_images}")
    print(f"{'class':<12} {'GT':>5} {'Pred':>5} {'P':>9} {'R':>9} {'AP50':>9}")
    for item in per_class:
        print(
            f"{item['class']:<12} {item['gt']:>5} {item['pred']:>5} "
            f"{item['precision']:>8.2%} {item['recall']:>8.2%} {item['ap50']:>8.2%}"
        )
    print(f"mAP@0.5: {mean_ap:.2%}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--onnx", type=Path, required=True)
    parser.add_argument("--calib-dataset", type=Path, required=True)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--split", choices=("val", "test"), default="test")
    parser.add_argument("--preprocess", choices=("both", "letterbox", "stretch"), default="both")
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--conf-threshold", type=float, default=0.25)
    parser.add_argument("--nms-threshold", type=float, default=0.45)
    parser.add_argument("--eval-iou-threshold", type=float, default=0.5)
    args = parser.parse_args()
    if args.imgsz != 640:
        raise ValueError("the current Rockchip export has a fixed 640x640 input")

    rknn = RKNN(verbose=False)
    rknn.config(
        mean_values=[[0, 0, 0]],
        std_values=[[255, 255, 255]],
        target_platform="rk3588",
        quantized_dtype="asymmetric_quantized-8",
        quantized_algorithm="normal",
        optimization_level=0,
    )
    if rknn.load_onnx(model=str(args.onnx)) != 0:
        raise RuntimeError("RKNN load_onnx failed")
    if rknn.build(do_quantization=True, dataset=str(args.calib_dataset)) != 0:
        raise RuntimeError("RKNN build failed")
    if rknn.init_runtime() != 0:
        raise RuntimeError("RKNN simulator init failed")
    try:
        modes = ("letterbox", "stretch") if args.preprocess == "both" else (args.preprocess,)
        for mode in modes:
            evaluate(rknn, args.dataset, mode, args)
    finally:
        rknn.release()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
