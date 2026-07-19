#!/usr/bin/env python3
import argparse
import csv
import time
from pathlib import Path

import cv2
import numpy as np
from rknn.api import RKNN


CLASSES = (
    "person", "bicycle", "car", "motorbike ", "aeroplane ", "bus ", "train", "truck ", "boat", "traffic light",
    "fire hydrant", "stop sign ", "parking meter", "bench", "bird", "cat", "dog ", "horse ", "sheep", "cow", "elephant",
    "bear", "zebra ", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", "kite",
    "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork", "knife ",
    "spoon", "bowl", "banana", "apple", "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza ", "donut", "cake", "chair", "sofa",
    "pottedplant", "bed", "diningtable", "toilet ", "tvmonitor", "laptop", "mouse", "remote ", "keyboard", "cell phone", "microwave ",
    "oven ", "toaster", "sink", "refrigerator ", "book", "clock", "vase", "scissors ", "teddy bear ", "hair drier", "toothbrush "
)

DESKTOP_CLASS_TO_ID = {
    "cell phone": 0,
    "cup": 1,
    "keyboard": 2,
    "mouse": 3,
    "laptop": 4,
    "tvmonitor": 4,
    "book": 5,
}
TARGET_MODEL_IDS = {idx for idx, label in enumerate(CLASSES) if label.strip() in DESKTOP_CLASS_TO_ID}
DESKTOP_MODEL_CLASSES = ("cell phone", "cup", "keyboard", "mouse", "laptop", "book")

DEFAULT_ANNOTATION_DIR = Path("/home/rambos/datasets/alertgateway_desktop6/annotation")
DEFAULT_RKNN = Path("/home/rambos/datasets/models/yolov8s_rockchip_dfl.rknn")
DEFAULT_ONNX = Path("/home/rambos/yolov8_models/onnx/yolov8s_official.onnx")
DEFAULT_CALIB_DATASET = Path("/home/rambos/calibration/dataset.txt")


def iou(box_a, box_b):
    xa = max(box_a[0], box_b[0])
    ya = max(box_a[1], box_b[1])
    xb = min(box_a[2], box_b[2])
    yb = min(box_a[3], box_b[3])
    inter = max(0.0, xb - xa) * max(0.0, yb - ya)
    if inter <= 0:
        return 0.0
    area_a = max(0.0, box_a[2] - box_a[0]) * max(0.0, box_a[3] - box_a[1])
    area_b = max(0.0, box_b[2] - box_b[0]) * max(0.0, box_b[3] - box_b[1])
    return inter / (area_a + area_b - inter + 1e-6)


def nms(dets, iou_thresh):
    dets = sorted(dets, key=lambda item: item["score"], reverse=True)
    keep = []
    suppressed = [False] * len(dets)
    for i, det in enumerate(dets):
        if suppressed[i]:
            continue
        keep.append(det)
        for j in range(i + 1, len(dets)):
            if suppressed[j]:
                continue
            if det["desktop_id"] != dets[j]["desktop_id"]:
                continue
            if iou(det["box"], dets[j]["box"]) > iou_thresh:
                suppressed[j] = True
    return keep


def postprocess_rockchip(outputs, orig_w, orig_h, model_w, model_h, conf_thresh, iou_thresh):
    scale_x = orig_w / float(model_w)
    scale_y = orig_h / float(model_h)
    candidates = []

    for branch in range(3):
        box_idx = branch * 3
        score_idx = box_idx + 1
        sum_idx = box_idx + 2

        box_tensor = outputs[box_idx][0]
        score_tensor = outputs[score_idx][0]
        sum_tensor = outputs[sum_idx][0][0]
        num_classes = score_tensor.shape[0]
        if num_classes == len(DESKTOP_MODEL_CLASSES):
            candidate_class_ids = range(num_classes)
        else:
            candidate_class_ids = TARGET_MODEL_IDS

        grid_h, grid_w = box_tensor.shape[1], box_tensor.shape[2]
        stride_x = model_w // grid_w
        stride_y = model_h // grid_h
        if stride_x != stride_y:
            raise ValueError(f"non-square stride: x={stride_x}, y={stride_y}")
        stride = stride_x
        dfl_len = box_tensor.shape[0] // 4

        for row in range(grid_h):
            for col in range(grid_w):
                if sum_tensor[row, col] < conf_thresh:
                    continue

                best_class = -1
                best_score = -1.0
                for class_id in candidate_class_ids:
                    score = float(score_tensor[class_id, row, col])
                    if score > conf_thresh and score > best_score:
                        best_score = score
                        best_class = class_id
                if best_class < 0:
                    continue

                distances = np.zeros(4, dtype=np.float32)
                for side in range(4):
                    logits = box_tensor[side * dfl_len : (side + 1) * dfl_len, row, col]
                    exp_logits = np.exp(logits - np.max(logits))
                    probs = exp_logits / np.sum(exp_logits)
                    distances[side] = np.sum(probs * np.arange(dfl_len, dtype=np.float32))

                x1 = (col + 0.5 - distances[0]) * stride * scale_x
                y1 = (row + 0.5 - distances[1]) * stride * scale_y
                x2 = (col + 0.5 + distances[2]) * stride * scale_x
                y2 = (row + 0.5 + distances[3]) * stride * scale_y

                x1 = max(0.0, min(float(orig_w), float(x1)))
                y1 = max(0.0, min(float(orig_h), float(y1)))
                x2 = max(0.0, min(float(orig_w), float(x2)))
                y2 = max(0.0, min(float(orig_h), float(y2)))
                if x2 - x1 < 2 or y2 - y1 < 2:
                    continue

                if num_classes == len(DESKTOP_MODEL_CLASSES):
                    model_label = DESKTOP_MODEL_CLASSES[best_class]
                else:
                    model_label = CLASSES[best_class].strip()
                candidates.append(
                    {
                        "model_class": best_class,
                        "label": model_label,
                        "desktop_id": DESKTOP_CLASS_TO_ID[model_label],
                        "score": best_score,
                        "box": [x1, y1, x2, y2],
                    }
                )

    return nms(candidates, iou_thresh)


def det_to_yolo_line(det, width, height):
    x1, y1, x2, y2 = det["box"]
    cx = ((x1 + x2) * 0.5) / width
    cy = ((y1 + y2) * 0.5) / height
    w = (x2 - x1) / width
    h = (y2 - y1) / height
    return f"{det['desktop_id']} {cx:.6f} {cy:.6f} {w:.6f} {h:.6f}"


def init_rknn(args):
    rknn = RKNN(verbose=False)
    if args.rknn:
        ret = rknn.load_rknn(str(args.rknn))
        if ret != 0:
            raise RuntimeError("RKNN load_rknn failed")
        ret = rknn.init_runtime()
        if ret != 0:
            raise RuntimeError("RKNN init_runtime failed")
        return rknn

    rknn.config(
        mean_values=[[0, 0, 0]],
        std_values=[[255, 255, 255]],
        target_platform="rk3588",
        quantized_dtype="asymmetric_quantized-8",
        quantized_algorithm="normal",
        optimization_level=0,
    )
    ret = rknn.load_onnx(model=str(args.onnx))
    if ret != 0:
        raise RuntimeError("RKNN load_onnx failed")
    ret = rknn.build(do_quantization=True, dataset=str(args.calib_dataset))
    if ret != 0:
        raise RuntimeError("RKNN build failed")
    ret = rknn.init_runtime()
    if ret != 0:
        raise RuntimeError("RKNN init_runtime failed")
    return rknn


class RknnRunner:
    def __init__(self, rknn):
        self.rknn = rknn

    def inference(self, img_rgb):
        return self.rknn.inference(inputs=[img_rgb])

    def release(self):
        self.rknn.release()


class OnnxRuntimeRunner:
    def __init__(self, onnx_path: Path):
        import onnxruntime as ort

        self.session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
        self.input_name = self.session.get_inputs()[0].name

    def inference(self, img_rgb):
        x = img_rgb.astype(np.float32) / 255.0
        x = np.transpose(x, (2, 0, 1))[None, :, :, :]
        return self.session.run(None, {self.input_name: x})

    def release(self):
        return None


def init_runner(args):
    if args.backend == "onnxruntime":
        return OnnxRuntimeRunner(args.onnx)
    return RknnRunner(init_rknn(args))


def main():
    parser = argparse.ArgumentParser(description="Generate YOLO pseudo labels for desktop6 annotation images.")
    parser.add_argument("--annotation-dir", type=Path, default=DEFAULT_ANNOTATION_DIR)
    parser.add_argument("--rknn", type=Path, default=DEFAULT_RKNN, help="Prebuilt rockchip_dfl RKNN model.")
    parser.add_argument("--onnx", type=Path, default=DEFAULT_ONNX)
    parser.add_argument("--calib-dataset", type=Path, default=DEFAULT_CALIB_DATASET)
    parser.add_argument("--backend", choices=("onnxruntime", "rknn"), default="onnxruntime")
    parser.add_argument("--model-width", type=int, default=640)
    parser.add_argument("--model-height", type=int, default=640)
    parser.add_argument("--conf-threshold", type=float, default=0.30)
    parser.add_argument("--nms-threshold", type=float, default=0.45)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing labels.")
    parser.add_argument("--write-empty", action="store_true", help="Write empty txt files when no detections are found.")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    images_dir = args.annotation_dir / "images"
    labels_dir = args.annotation_dir / "labels"
    manifest_path = args.annotation_dir / "pseudo_label_manifest.csv"
    images = sorted(images_dir.glob("*.jpg"))
    if args.limit > 0:
        images = images[: args.limit]

    print(f"images: {len(images)}")
    print(f"labels_dir: {labels_dir}")
    print(f"overwrite: {args.overwrite}")
    print(f"dry_run: {args.dry_run}")

    runner = init_runner(args)
    labels_dir.mkdir(parents=True, exist_ok=True)

    rows = []
    written = 0
    skipped_existing = 0
    empty_predictions = 0
    total_dets = 0
    t0 = time.time()

    try:
        for idx, image_path in enumerate(images, start=1):
            label_path = labels_dir / f"{image_path.stem}.txt"
            if label_path.exists() and not args.overwrite:
                skipped_existing += 1
                rows.append(
                    {
                        "image": image_path.name,
                        "status": "skipped_existing",
                        "detections": "",
                        "labels": "",
                    }
                )
                continue

            img = cv2.imread(str(image_path))
            if img is None:
                rows.append({"image": image_path.name, "status": "missing_image", "detections": "", "labels": ""})
                continue
            orig_h, orig_w = img.shape[:2]
            resized = cv2.resize(img, (args.model_width, args.model_height))
            rgb = cv2.cvtColor(resized, cv2.COLOR_BGR2RGB)
            outputs = runner.inference(rgb)
            dets = postprocess_rockchip(
                outputs,
                orig_w,
                orig_h,
                args.model_width,
                args.model_height,
                args.conf_threshold,
                args.nms_threshold,
            )
            lines = [det_to_yolo_line(det, orig_w, orig_h) for det in dets]
            labels = ";".join(f"{det['label']}:{det['score']:.3f}" for det in dets)
            total_dets += len(dets)
            if not dets:
                empty_predictions += 1

            if lines or args.write_empty:
                status = "dry_run" if args.dry_run else "written"
            else:
                status = "no_detection_unwritten"

            # A browser annotator may save the same image while this batch is
            # running. Preserve that human label unless overwrite was asked.
            if label_path.exists() and not args.overwrite:
                skipped_existing += 1
                rows.append(
                    {
                        "image": image_path.name,
                        "status": "skipped_saved_during_run",
                        "detections": str(len(dets)),
                        "labels": labels,
                    }
                )
                continue

            if not args.dry_run and (lines or args.write_empty):
                label_path.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")
                written += 1
            rows.append(
                {
                    "image": image_path.name,
                    "status": status,
                    "detections": str(len(dets)),
                    "labels": labels,
                }
            )
            if idx % 25 == 0 or idx == len(images):
                print(f"processed {idx}/{len(images)} written={written} skipped_existing={skipped_existing} dets={total_dets}")
    finally:
        runner.release()

    if not args.dry_run:
        with manifest_path.open("w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=["image", "status", "detections", "labels"])
            writer.writeheader()
            writer.writerows(rows)

    elapsed = time.time() - t0
    print(f"written: {written}")
    print(f"skipped_existing: {skipped_existing}")
    print(f"empty_predictions: {empty_predictions}")
    print(f"total_detections: {total_dets}")
    print(f"elapsed_sec: {elapsed:.1f}")
    if not args.dry_run:
        print(f"manifest: {manifest_path}")


if __name__ == "__main__":
    main()
