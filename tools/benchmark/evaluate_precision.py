#!/usr/bin/env python3
import os
import sys
import json
import numpy as np
import cv2
import time
import argparse
from rknn.api import RKNN

# 1. 类别名称与映射定义
CLASSES = (
    "person", "bicycle", "car", "motorbike ", "aeroplane ", "bus ", "train", "truck ", "boat", "traffic light",
    "fire hydrant", "stop sign ", "parking meter", "bench", "bird", "cat", "dog ", "horse ", "sheep", "cow", "elephant",
    "bear", "zebra ", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis", "snowboard", "sports ball", "kite",
    "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup", "fork", "knife ",
    "spoon", "bowl", "banana", "apple", "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza ", "donut", "cake", "chair", "sofa",
    "pottedplant", "bed", "diningtable", "toilet ", "tvmonitor", "laptop", "mouse", "remote ", "keyboard", "cell phone", "microwave ",
    "oven ", "toaster", "sink", "refrigerator ", "book", "clock", "vase", "scissors ", "teddy bear ", "hair drier", "toothbrush "
)

COCO_ID_LIST = [
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 27, 28, 31, 32, 33, 34,
    35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    64, 65, 67, 70, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 84, 85, 86, 87, 88, 89, 90
]

COCO_TO_YOLO_CLASS = {coco_id: idx for idx, coco_id in enumerate(COCO_ID_LIST)}

# 我们业务重点关注的 6 个类别名和对应的 class_id
TARGET_CLASSES = ["cell phone", "cup", "keyboard", "mouse", "laptop", "book"]
TARGET_IDS = [CLASSES.index(cls) for cls in TARGET_CLASSES]

# 2. 基础数学与几何函数
def iou(boxA, boxB):
    xA = max(boxA[0], boxB[0])
    yA = max(boxA[1], boxB[1])
    xB = min(boxA[2], boxB[2])
    yB = min(boxA[3], boxB[3])
    interArea = max(0.0, xB - xA) * max(0.0, yB - yA)
    if interArea == 0.0:
        return 0.0
    boxAArea = (boxA[2] - boxA[0]) * (boxA[3] - boxA[1])
    boxBArea = (boxB[2] - boxB[0]) * (boxB[3] - boxB[1])
    return interArea / float(boxAArea + boxBArea - interArea + 1e-6)

def nms(dets, iou_thresh):
    if len(dets) == 0:
        return []
    dets = sorted(dets, key=lambda x: x['score'], reverse=True)
    keep = []
    suppressed = [False] * len(dets)
    for i in range(len(dets)):
        if suppressed[i]:
            continue
        keep.append(dets[i])
        for j in range(i + 1, len(dets)):
            if not suppressed[j] and dets[i]['class_id'] == dets[j]['class_id']:
                if iou(dets[i]['box'], dets[j]['box']) > iou_thresh:
                    suppressed[j] = True
    return keep

# 3. 两种后处理算法实现（严格对齐 C++）

# (1) decoded 双输出后处理
def postprocess_decoded(outputs, orig_w, orig_h, model_w, model_h, conf_thresh, iou_thresh):
    # outputs[0]: boxes (1, 4, 8400)
    # outputs[1]: class_probs (1, 80, 8400) (ONNX 图里已经过 Sigmoid)
    boxes = outputs[0][0] # (4, 8400)
    cls_probs = outputs[1][0] # (80, 8400)
    
    n_anchors = boxes.shape[1]
    sx = orig_w / float(model_w)
    sy = orig_h / float(model_h)
    
    candidates = []
    for a in range(n_anchors):
        # 寻找最高分类别
        best_score = -1.0
        best_class = -1
        for c in range(80):
            score = cls_probs[c, a]
            if score > best_score:
                best_score = score
                best_class = c
                
        if best_score < conf_thresh:
            continue
            
        label = CLASSES[best_class]
        # 只保留 6 类目标
        if label not in TARGET_CLASSES:
            continue
            
        cx = boxes[0, a]
        cy = boxes[1, a]
        w = boxes[2, a]
        h = boxes[3, a]
        
        x1 = (cx - w * 0.5) * sx
        y1 = (cy - h * 0.5) * sy
        x2 = (cx + w * 0.5) * sx
        y2 = (cy + h * 0.5) * sy
        
        candidates.append({
            "class_id": best_class,
            "label": label,
            "score": float(best_score),
            "box": [x1, y1, x2, y2]
        })
        
    return nms(candidates, iou_thresh)

# (2) rockchip_dfl 九输出后处理
def postprocess_rockchip(outputs, orig_w, orig_h, model_w, model_h, conf_thresh, iou_thresh):
    # outputs 包含 9 个张量
    scale_x = orig_w / float(model_w)
    scale_y = orig_h / float(model_h)
    candidates = []
    
    for branch in range(3):
        box_idx = branch * 3
        score_idx = box_idx + 1
        sum_idx = box_idx + 2
        
        box_tensor = outputs[box_idx][0]       # (64, H, W)
        score_tensor = outputs[score_idx][0]   # (80, H, W)
        sum_tensor = outputs[sum_idx][0][0]    # (H, W)
        
        H, W = box_tensor.shape[1], box_tensor.shape[2]
        if model_h % H != 0 or model_w % W != 0:
            raise ValueError(f"输出网格 {W}x{H} 与模型输入 {model_w}x{model_h} 不整除")
        stride_y = model_h // H
        stride_x = model_w // W
        if stride_x != stride_y:
            raise ValueError(f"输出网格 {W}x{H} 推导出非等比 stride: x={stride_x}, y={stride_y}")
        stride = stride_y
        dfl_len = box_tensor.shape[0] // 4 # 16
        
        for row in range(H):
            for col in range(W):
                # 快速过滤置信度求和分支
                if sum_tensor[row, col] < conf_thresh:
                    continue
                    
                # 寻找最大概率的类别
                best_class = -1
                best_score = -1.0
                for class_id in range(80):
                    score = score_tensor[class_id, row, col]
                    if score > conf_thresh and score > best_score:
                        best_score = score
                        best_class = class_id
                        
                if best_class < 0:
                    continue
                    
                label = CLASSES[best_class]
                if label not in TARGET_CLASSES:
                    continue
                    
                # 提取并计算 DFL 距离
                distances = np.zeros(4)
                for side in range(4):
                    dfl_logits = box_tensor[side * dfl_len : (side + 1) * dfl_len, row, col]
                    # Softmax
                    max_logit = np.max(dfl_logits)
                    exp_logits = np.exp(dfl_logits - max_logit)
                    softmax_probs = exp_logits / np.sum(exp_logits)
                    distances[side] = np.sum(softmax_probs * np.arange(dfl_len))
                    
                # 映射坐标回到原始图
                x1 = (col + 0.5 - distances[0]) * stride * scale_x
                y1 = (row + 0.5 - distances[1]) * stride * scale_y
                x2 = (col + 0.5 + distances[2]) * stride * scale_x
                y2 = (row + 0.5 + distances[3]) * stride * scale_y
                
                candidates.append({
                    "class_id": best_class,
                    "label": label,
                    "score": float(best_score),
                    "box": [x1, y1, x2, y2]
                })
                
    return nms(candidates, iou_thresh)

# 4. 加载 COCO 子集标注
def load_coco_subset_gt(subset_txt_path, anno_path):
    if not os.path.exists(subset_txt_path):
        raise FileNotFoundError(f"Missing subset list at: {subset_txt_path}")
        
    with open(subset_txt_path) as f:
        img_paths = [line.strip() for line in f if line.strip()]
        
    img_names = [os.path.basename(p) for p in img_paths]
    img_ids = [int(name.split('.')[0]) for name in img_names]
    id_to_name = {img_id: name for img_id, name in zip(img_ids, img_names)}
    
    if not os.path.exists(anno_path):
        raise FileNotFoundError(f"Annotations not found at {anno_path}.")
        
    with open(anno_path) as f:
        coco_data = json.load(f)
        
    # 过滤对应的图片宽度高度信息
    images_info = {}
    for img in coco_data['images']:
        if img['id'] in id_to_name:
            images_info[id_to_name[img['id']]] = {
                "width": img['width'],
                "height": img['height'],
                "image_id": img['id']
            }
            
    # 过滤 annotations
    gt_by_file = {name: [] for name in img_names}
    for ann in coco_data['annotations']:
        img_id = ann['image_id']
        if img_id in id_to_name:
            filename = id_to_name[img_id]
            coco_cat_id = ann['category_id']
            if coco_cat_id in COCO_TO_YOLO_CLASS:
                yolo_cat_id = COCO_TO_YOLO_CLASS[coco_cat_id]
                label = CLASSES[yolo_cat_id]
                
                # 只评测这六个目标类别
                if label in TARGET_CLASSES:
                    bbox = ann['bbox'] # [x, y, w, h]
                    x1 = bbox[0]
                    y1 = bbox[1]
                    x2 = bbox[0] + bbox[2]
                    y2 = bbox[1] + bbox[3]
                    
                    gt_by_file[filename].append({
                        "class_id": yolo_cat_id,
                        "label": label,
                        "box": [x1, y1, x2, y2]
                    })
                    
    return gt_by_file, images_info

# 5. 加载真实桌面场景标注
def load_desktop_gt():
    # 统一使用相对 evaluate_precision.py 所在目录的相对定位
    desktop_gt_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "desktop_gt.json")
    if not os.path.exists(desktop_gt_path):
        raise FileNotFoundError(f"Missing desktop GT file: {desktop_gt_path}")
    with open(desktop_gt_path) as f:
        return json.load(f)

# 6. 精度评估计算
def calculate_precision_metrics(gt_dataset, pred_dataset, eval_iou_thresh=0.5):
    all_ap = []
    results = {}
    
    for target_cls in TARGET_CLASSES:
        target_id = CLASSES.index(target_cls)
        
        # 整理该类的所有 GT 框和预测框
        class_gt = []
        for filename, gts in gt_dataset.items():
            for gt in gts:
                if gt['class_id'] == target_id:
                    class_gt.append({
                        "image_id": filename,
                        "box": gt['box'],
                        "matched": False
                    })
                    
        class_pred = []
        for filename, preds in pred_dataset.items():
            for pred in preds:
                if pred['class_id'] == target_id:
                    class_pred.append({
                        "image_id": filename,
                        "box": pred['box'],
                        "score": pred['score']
                    })
                    
        # 按照 score 从高到低排序
        class_pred = sorted(class_pred, key=lambda x: x['score'], reverse=True)
        num_gts = len(class_gt)
        
        if num_gts == 0:
            results[target_cls] = {"ap": 0.0, "precision": 0.0, "recall": 0.0, "gt_count": 0}
            continue
            
        tp = np.zeros(len(class_pred))
        fp = np.zeros(len(class_pred))
        
        gt_by_img = {}
        for gt in class_gt:
            gt_by_img.setdefault(gt['image_id'], []).append(gt)
            
        for idx, pred in enumerate(class_pred):
            img_id = pred['image_id']
            pbox = pred['box']
            
            best_iou = -1.0
            best_gt = None
            
            if img_id in gt_by_img:
                for gt in gt_by_img[img_id]:
                    if gt['matched']:
                        continue
                    iou_val = iou(pbox, gt['box'])
                    if iou_val > best_iou:
                        best_iou = iou_val
                        best_gt = gt
                        
            if best_iou >= eval_iou_thresh:
                tp[idx] = 1
                best_gt['matched'] = True
            else:
                fp[idx] = 1
                
        tp_cumsum = np.cumsum(tp)
        fp_cumsum = np.cumsum(fp)
        
        precisions = tp_cumsum / (tp_cumsum + fp_cumsum + 1e-6)
        recalls = tp_cumsum / num_gts
        
        mrec = np.concatenate(([0.0], recalls, [1.0]))
        mpre = np.concatenate(([1.0], precisions, [0.0]))
        
        for i in range(len(mpre) - 2, -1, -1):
            mpre[i] = max(mpre[i], mpre[i + 1])
            
        i_indices = np.where(mrec[1:] != mrec[:-1])[0]
        ap = np.sum((mrec[i_indices + 1] - mrec[i_indices]) * mpre[i_indices + 1])
        all_ap.append(ap)
        
        final_prec = precisions[-1] if len(precisions) > 0 else 0.0
        final_rec = recalls[-1] if len(recalls) > 0 else 0.0
        
        results[target_cls] = {
            "ap": float(ap),
            "precision": float(final_prec),
            "recall": float(final_rec),
            "gt_count": num_gts
        }
        
    mAP = np.mean(all_ap) if len(all_ap) > 0 else 0.0
    return mAP, results

# 7. 主流程：量化编译并在 Simulator 评估
def evaluate_model(onnx_path, postprocess_fn, gt_dataset, images_info, dataset_txt,
                   coco_subset_dir, desktop_frames_dir, model_w, model_h,
                   conf_thresh, iou_thresh, is_decoded=True):
    rknn = RKNN(verbose=False)
    rknn.config(
        mean_values=[[0, 0, 0]],
        std_values=[[255, 255, 255]],
        target_platform="rk3588",
        quantized_dtype="asymmetric_quantized-8",
        quantized_algorithm="normal",
        optimization_level=0
    )
    
    print(f"Loading ONNX: {onnx_path}")
    if is_decoded:
        # 生产双输出模型指定特定输出头
        ret = rknn.load_onnx(model=onnx_path, outputs=['/model.22/Mul_2_output_0', '/model.22/Sigmoid_output_0'])
    else:
        # 官方优化模型直接加载全部
        ret = rknn.load_onnx(model=onnx_path)
        
    if ret != 0:
        raise RuntimeError("Load ONNX failed")
        
    print("Building INT8 RKNN Model on Simulator...")
    ret = rknn.build(do_quantization=True, dataset=dataset_txt)
    if ret != 0:
        raise RuntimeError("RKNN build failed")
        
    ret = rknn.init_runtime()
    if ret != 0:
        raise RuntimeError("RKNN init_runtime failed")
        
    predictions = {}
    print("Running batch evaluation on images...")
    
    for filename in gt_dataset.keys():
        if filename.startswith("det_A_"):
            img_path = os.path.join(desktop_frames_dir, filename)
            orig_w, orig_h = 640, 480
        else:
            img_path = os.path.join(coco_subset_dir, filename)
            info = images_info[filename]
            orig_w, orig_h = info['width'], info['height']
            
        img = cv2.imread(img_path)
        if img is None:
            print(f"Error: image not found at {img_path}")
            continue
            
        # 预处理与 C++ 对齐
        img_resized = cv2.resize(img, (model_w, model_h))
        img_rgb = cv2.cvtColor(img_resized, cv2.COLOR_BGR2RGB)
        
        outputs = rknn.inference(inputs=[img_rgb])
        
        preds = postprocess_fn(outputs, orig_w, orig_h, model_w, model_h, conf_thresh, iou_thresh)
        predictions[filename] = preds
        
    rknn.release()
    return predictions

def main():
    parser = argparse.ArgumentParser(description="YOLOv8s-RK3588 Accuracy Simulator Evaluation")
    parser.add_argument("--yolo-onnx", type=str, default="/home/rambos/yolov8s.onnx",
                        help="Path to production YOLOv8s ONNX model")
    parser.add_argument("--official-onnx", type=str, default="/home/rambos/yolov8s_official.onnx",
                        help="Path to official YOLOv8s ONNX model")
    parser.add_argument("--calib-dataset", type=str, default="/home/rambos/calibration/dataset.txt",
                        help="Path to calibration dataset.txt path mapping")
    parser.add_argument("--coco-subset-txt", type=str, default="/home/rambos/arm_test/rknn_model_zoo/datasets/COCO/coco_subset_20.txt",
                        help="Path to coco_subset_20.txt image filenames")
    parser.add_argument("--coco-subset-dir", type=str, default="/home/rambos/arm_test/rknn_model_zoo/datasets/COCO/subset",
                        help="Directory for COCO subset local images")
    parser.add_argument("--coco-anno-json", type=str, default="/tmp/annotations/instances_val2017.json",
                        help="Path to instances_val2017.json annotations")
    parser.add_argument("--desktop-frames-dir", type=str, default="/home/rambos/.gemini/antigravity-ide/brain/6324f92e-a4fc-45b4-bb2d-f663185ef4b6/scratch/frames/detection",
                        help="Directory containing local desktop frames")
    parser.add_argument("--conf-threshold", type=float, default=0.25,
                        help="Confidence threshold for postprocessing")
    parser.add_argument("--nms-threshold", type=float, default=0.45,
                        help="NMS IoU threshold for postprocessing")
    parser.add_argument("--eval-iou-threshold", type=float, default=0.50,
                        help="IoU threshold for evaluation match (e.g., mAP@0.5)")
    parser.add_argument("--model-width", type=int, default=640,
                        help="Model input width used by preprocessing")
    parser.add_argument("--model-height", type=int, default=640,
                        help="Model input height used by preprocessing")
    parser.add_argument("--model-mode", choices=("both", "decoded", "rockchip_dfl"), default="both",
                        help="Model(s) to evaluate (default: both)")
    args = parser.parse_args()
    if args.model_width <= 0 or args.model_height <= 0:
        raise ValueError("--model-width and --model-height must be positive")

    print("====================================================")
    print("  YOLOv8s-RK3588 Quantized Accuracy Evaluation Suite")
    print("====================================================\n")
    
    # (1) 加载验证集
    print(">> Loading datasets...")
    coco_gt, coco_images_info = load_coco_subset_gt(args.coco_subset_txt, args.coco_anno_json)
    desktop_gt = load_desktop_gt()
    
    combined_gt = {}
    combined_gt.update(coco_gt)
    combined_gt.update(desktop_gt)
    
    print(f"Loaded {len(coco_gt)} COCO subset images and {len(desktop_gt)} desktop scenario frames.")
    print(f"Total evaluation size: {len(combined_gt)} frames.\n")
    
    decoded_preds = None
    rockchip_preds = None

    if args.model_mode in ("both", "decoded"):
        print(">> Evaluating Production [decoded] Model...")
        t0 = time.time()
        decoded_preds = evaluate_model(
            onnx_path=args.yolo_onnx,
            postprocess_fn=postprocess_decoded,
            gt_dataset=combined_gt,
            images_info=coco_images_info,
            dataset_txt=args.calib_dataset,
            coco_subset_dir=args.coco_subset_dir,
            desktop_frames_dir=args.desktop_frames_dir,
            model_w=args.model_width,
            model_h=args.model_height,
            conf_thresh=args.conf_threshold,
            iou_thresh=args.nms_threshold,
            is_decoded=True
        )
        t1 = time.time()
        print(f"Production [decoded] Model evaluation completed in {t1 - t0:.1f} seconds.\n")
    
    if args.model_mode in ("both", "rockchip_dfl"):
        print(">> Evaluating Optimized [rockchip_dfl] Model...")
        t0 = time.time()
        rockchip_preds = evaluate_model(
            onnx_path=args.official_onnx,
            postprocess_fn=postprocess_rockchip,
            gt_dataset=combined_gt,
            images_info=coco_images_info,
            dataset_txt=args.calib_dataset,
            coco_subset_dir=args.coco_subset_dir,
            desktop_frames_dir=args.desktop_frames_dir,
            model_w=args.model_width,
            model_h=args.model_height,
            conf_thresh=args.conf_threshold,
            iou_thresh=args.nms_threshold,
            is_decoded=False
        )
        t1 = time.time()
        print(f"Optimized [rockchip_dfl] Model evaluation completed in {t1 - t0:.1f} seconds.\n")
    
    # (4) 计算精度指标并输出报表
    print(f">> Calculating Accuracy metrics (IoU threshold = {args.eval_iou_threshold:.2f})...")
    
    reports = []
    if decoded_preds is not None:
        coco_map, coco_res = calculate_precision_metrics(coco_gt, {k: decoded_preds[k] for k in coco_gt.keys()}, args.eval_iou_threshold)
        desk_map, desk_res = calculate_precision_metrics(desktop_gt, {k: decoded_preds[k] for k in desktop_gt.keys()}, args.eval_iou_threshold)
        reports.append(("Production [decoded]", coco_map, coco_res, desk_map, desk_res))
    if rockchip_preds is not None:
        coco_map, coco_res = calculate_precision_metrics(coco_gt, {k: rockchip_preds[k] for k in coco_gt.keys()}, args.eval_iou_threshold)
        desk_map, desk_res = calculate_precision_metrics(desktop_gt, {k: rockchip_preds[k] for k in desktop_gt.keys()}, args.eval_iou_threshold)
        reports.append(("Optimized [rockchip_dfl]", coco_map, coco_res, desk_map, desk_res))

    print("\n" + "="*80)
    print("                    ACCURACY METRICS REPORT")
    print("="*80)
    print(f" Model input: {args.model_width}x{args.model_height}")
    print("-"*80)

    for model_name, coco_map, coco_res, desk_map, desk_res in reports:
        print(f" [{model_name}]")
        print(f"  COCO Subset mAP@0.5       | {coco_map:>20.2%}")
        for cls in TARGET_CLASSES:
            info = coco_res[cls]
            print(f"  - {cls:<18} Recall  | {info['recall']:>20.2%}")
            print(f"  - {cls:<18} Precision| {info['precision']:>20.2%}")
        print(f"  Desktop Scenario mAP@0.5  | {desk_map:>20.2%}")
        for cls in ["laptop", "cup"]:
            info = desk_res[cls]
            print(f"  - {cls:<18} Recall  | {info['recall']:>20.2%}")
            print(f"  - {cls:<18} Precision| {info['precision']:>20.2%}")
        print("-"*80)
    print("="*80 + "\n")

if __name__ == "__main__":
    main()
