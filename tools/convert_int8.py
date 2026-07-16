#!/usr/bin/env python3
"""
将 yolov8s.onnx 转换为 INT8 量化 rknn 模型。
校准数据集：默认 ~/calibration/calib_*.png，图片尺寸需与模型输入尺寸和推理预处理一致。

用法：
    python3 tools/convert_int8.py
    python3 tools/convert_int8.py --optimization-level 2 \
        --output ~/exp002/yolov8s_opt2.rknn
    python3 tools/convert_int8.py --onnx ~/yolov8s_640x480.onnx \
        --calib-dir ~/calibration_640x480 \
        --dataset-txt ~/calibration_640x480/dataset.txt \
        --output-layout rockchip_dfl \
        --output ~/yolov8_models/rknn/yolov8s_rockchip_dfl_640x480.rknn
"""
import argparse
import os
import glob
from rknn.api import RKNN

ONNX_MODEL    = os.path.expanduser("~/yolov8s.onnx")
RKNN_MODEL    = os.path.expanduser("~/yolov8_models/rknn/yolov8s_int8.rknn")
CALIB_DIR     = os.path.expanduser("~/calibration")
PLATFORM      = "rk3588"
MEAN_VALUES   = [[0, 0, 0]]        # YOLOv8 预处理：/255，均值=0
STD_VALUES    = [[255, 255, 255]]  # 标准差=255 等价于 /255 归一化


DATASET_TXT   = os.path.expanduser("~/calibration/dataset.txt")
VERBOSE_LOG   = "/tmp/rknn_convert_verbose.log"


def parse_args():
    parser = argparse.ArgumentParser(description="Convert YOLOv8s ONNX to an INT8 RKNN model")
    parser.add_argument(
        "--onnx",
        default=ONNX_MODEL,
        help=f"input ONNX path (default: {ONNX_MODEL})",
    )
    parser.add_argument(
        "--optimization-level",
        type=int,
        choices=range(4),
        default=0,
        metavar="{0,1,2,3}",
        help="RKNN graph optimization level (default: 0)",
    )
    parser.add_argument(
        "--output",
        default=RKNN_MODEL,
        help=f"output RKNN path (default: {RKNN_MODEL})",
    )
    parser.add_argument(
        "--calib-dir",
        default=CALIB_DIR,
        help=f"directory containing calib_*.png (default: {CALIB_DIR})",
    )
    parser.add_argument(
        "--dataset-txt",
        default=DATASET_TXT,
        help=f"calibration dataset list path (default: {DATASET_TXT})",
    )
    parser.add_argument(
        "--dataset-limit",
        type=int,
        default=150,
        help="maximum calibration images to include (default: 150)",
    )
    parser.add_argument(
        "--output-layout",
        choices=("decoded", "rockchip_dfl"),
        default="decoded",
        help="decoded exports two explicit heads; rockchip_dfl keeps official YOLO outputs (default: decoded)",
    )
    parser.add_argument(
        "--verbose-log",
        default=VERBOSE_LOG,
        help=f"RKNN verbose log path (default: {VERBOSE_LOG})",
    )
    return parser.parse_args()


def print_non_int8_layers(log_path):
    """读取 verbose 日志，只打印精度高于 int8 的层（fallback / float / fp16）。"""
    if not os.path.exists(log_path):
        print("[精度检查] 未生成 verbose 日志，跳过检查。")
        return
    keywords = ["fp16", "fp32", "fallback", "non-quantized", "hybrid", "not support quant"]
    # "float32 → int8" 是正向量化消息，不算 fallback
    exclude = ["changed from 'float32' to 'int8'", "changed from float32 to int8"]
    found = []
    with open(log_path) as f:
        for line in f:
            low = line.lower()
            if any(e in low for e in exclude):
                continue
            if any(k in low for k in keywords):
                found.append(line.rstrip())
    if found:
        print(f"\n[精度检查] 发现 {len(found)} 行非 INT8 层信息：")
        for l in found:
            print(f"  !! {l}")
    else:
        print("\n[精度检查] 所有层均为 INT8，无 float/fp16 fallback。")


def make_dataset_txt(calib_dir, txt_path, limit=150):
    calib_dir = os.path.expanduser(calib_dir)
    txt_path = os.path.expanduser(txt_path)
    paths = sorted(glob.glob(os.path.join(calib_dir, "calib_*.png")))[:limit]
    if not paths:
        raise FileNotFoundError(f"未找到校准图片：{calib_dir}/calib_*.png")
    os.makedirs(os.path.dirname(os.path.abspath(txt_path)), exist_ok=True)
    contents = "".join(p + "\n" for p in paths)
    current_contents = None
    if os.path.exists(txt_path):
        with open(txt_path) as f:
            current_contents = f.read()
    if current_contents != contents:
        with open(txt_path, "w") as f:
            f.write(contents)
    print(f"校准数据集：{len(paths)} 张 → {txt_path}")
    return txt_path


def main():
    args = parse_args()
    onnx_path = os.path.expanduser(args.onnx)
    output_path = os.path.expanduser(args.output)
    verbose_log = os.path.expanduser(args.verbose_log)
    output_dir = os.path.dirname(os.path.abspath(output_path))
    log_dir = os.path.dirname(os.path.abspath(verbose_log))
    os.makedirs(output_dir, exist_ok=True)
    os.makedirs(log_dir, exist_ok=True)

    rknn = RKNN(verbose=True, verbose_file=verbose_log)

    # 1. 配置：INT8 量化 + RK3588 平台
    print(f">> 配置模型（layout={args.output_layout}, optimization_level={args.optimization_level}）...")
    rknn.config(
        mean_values=MEAN_VALUES,
        std_values=STD_VALUES,
        target_platform=PLATFORM,
        quantized_dtype="asymmetric_quantized-8",
        quantized_algorithm="normal",
        optimization_level=args.optimization_level,
    )

    # 2. 加载 ONNX 模型
    # 在 concat 前拆出 box（已解码到像素空间）和 class（已过 Sigmoid）两个分支单独
    # 作为输出，分别量化校准。原始单一 84 通道合并输出共享一组 scale，对 box 坐标
    # （范围 0-640）和 class 概率（范围 0-1）这种数量级悬殊的数据用同一个 scale
    # 量化，会把 class 概率的精度完全压没（反量化回来全部变成 0）。
    print(f">> 加载 ONNX：{onnx_path}")
    if args.output_layout == "decoded":
        ret = rknn.load_onnx(
            model=onnx_path,
            outputs=['/model.22/Mul_2_output_0', '/model.22/Sigmoid_output_0'],
        )
    else:
        ret = rknn.load_onnx(model=onnx_path)
    if ret != 0:
        raise RuntimeError(f"load_onnx 失败：{ret}")

    # 3. INT8 量化构建（传入 dataset.txt 文件路径）
    print(">> 开始量化构建（耗时约 3-5 分钟）...")
    dataset_txt = make_dataset_txt(args.calib_dir, args.dataset_txt, args.dataset_limit)
    ret = rknn.build(do_quantization=True, dataset=dataset_txt)
    if ret != 0:
        raise RuntimeError(f"build 失败：{ret}")

    # 4. 导出 rknn 模型
    print(f">> 导出模型：{output_path}")
    ret = rknn.export_rknn(output_path)
    if ret != 0:
        raise RuntimeError(f"export_rknn 失败：{ret}")

    # 检查是否有层 fallback 到 float/fp16
    print_non_int8_layers(verbose_log)

    rknn.release()
    print(f"\n完成！INT8 模型已保存至：{output_path}")
    print("后续：scp 上传到板子，并按 output_layout 配置匹配的模型文件名。")


if __name__ == "__main__":
    main()
