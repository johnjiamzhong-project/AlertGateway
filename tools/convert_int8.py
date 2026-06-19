#!/usr/bin/env python3
"""
将 yolov8s.onnx 转换为 INT8 量化 rknn 模型。
校准数据集：~/calibration/calib_*.png（640×640 RGB，与推理预处理一致）

用法：
    python3 tools/convert_int8.py
"""
import os
import glob
import numpy as np
import cv2
from rknn.api import RKNN

ONNX_MODEL    = os.path.expanduser("~/yolov8s.onnx")
RKNN_MODEL    = os.path.expanduser("~/yolov8s_int8.rknn")
CALIB_DIR     = os.path.expanduser("~/calibration")
PLATFORM      = "rk3588"
MEAN_VALUES   = [[0, 0, 0]]        # YOLOv8 预处理：/255，均值=0
STD_VALUES    = [[255, 255, 255]]  # 标准差=255 等价于 /255 归一化


DATASET_TXT   = os.path.expanduser("~/calibration/dataset.txt")
VERBOSE_LOG   = "/tmp/rknn_convert_verbose.log"


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
    paths = sorted(glob.glob(os.path.join(calib_dir, "calib_*.png")))[:limit]
    if not paths:
        raise FileNotFoundError(f"未找到校准图片：{calib_dir}/calib_*.png")
    with open(txt_path, "w") as f:
        for p in paths:
            f.write(p + "\n")
    print(f"校准数据集：{len(paths)} 张 → {txt_path}")
    return txt_path


def main():
    rknn = RKNN(verbose=True, verbose_file=VERBOSE_LOG)

    # 1. 配置：INT8 量化 + RK3588 平台
    print(">> 配置模型...")
    rknn.config(
        mean_values=MEAN_VALUES,
        std_values=STD_VALUES,
        target_platform=PLATFORM,
        quantized_dtype="asymmetric_quantized-8",
        quantized_algorithm="normal",
        optimization_level=0,
    )

    # 2. 加载 ONNX 模型
    # 在 concat 前拆出 box（已解码到像素空间）和 class（已过 Sigmoid）两个分支单独
    # 作为输出，分别量化校准。原始单一 84 通道合并输出共享一组 scale，对 box 坐标
    # （范围 0-640）和 class 概率（范围 0-1）这种数量级悬殊的数据用同一个 scale
    # 量化，会把 class 概率的精度完全压没（反量化回来全部变成 0）。
    print(f">> 加载 ONNX：{ONNX_MODEL}")
    ret = rknn.load_onnx(model=ONNX_MODEL,
                          outputs=['/model.22/Mul_2_output_0', '/model.22/Sigmoid_output_0'])
    if ret != 0:
        raise RuntimeError(f"load_onnx 失败：{ret}")

    # 3. INT8 量化构建（传入 dataset.txt 文件路径）
    print(">> 开始量化构建（耗时约 3-5 分钟）...")
    dataset_txt = make_dataset_txt(CALIB_DIR, DATASET_TXT)
    ret = rknn.build(do_quantization=True, dataset=dataset_txt)
    if ret != 0:
        raise RuntimeError(f"build 失败：{ret}")

    # 4. 导出 rknn 模型
    print(f">> 导出模型：{RKNN_MODEL}")
    ret = rknn.export_rknn(RKNN_MODEL)
    if ret != 0:
        raise RuntimeError(f"export_rknn 失败：{ret}")

    # 检查是否有层 fallback 到 float/fp16
    print_non_int8_layers(VERBOSE_LOG)

    rknn.release()
    print(f"\n完成！INT8 模型已保存至：{RKNN_MODEL}")
    print("后续：scp 上传到板子，替换 ~/AlertGateway/model/yolov8s.rknn")


if __name__ == "__main__":
    main()
