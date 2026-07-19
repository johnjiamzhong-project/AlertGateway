AlertGateway 模型结构实验工件
==============================

本目录保存桌面六类 YOLOv8s 的结构分析结果、候选模型 YAML 和优化方案草稿，
用于主机侧剪枝、蒸馏及算子替换实验。它们不是板端运行时资源，也不包含 .pt、
.onnx 或 .rknn 权重。

文件分类
--------

1. 基线与结构分析
   - yolov8s_baseline_model.yaml：六类 YOLOv8s 基线结构。
   - baseline_channel_importance.txt：基于 BN gamma 的通道重要性分析。
   - yolov8s_layer_dependency.tsv：顶层模块依赖关系和参数统计。
   - yolov8s_c2f_internal.tsv：C2f 内部卷积、通道和参数明细。

2. 候选结构
   - yolov8s_pruned5_model.yaml：约 5% 规模方向的候选结构。
   - yolov8s_pruned5r_model.yaml：pruned5 的变体候选。
   - yolov8s_pruned10_model.yaml：约 10% 规模方向的候选结构。
   - yolov8s_depth4_reduced_model.yaml：减少深度的候选结构。
   - yolov8s_relu6_model.yaml：将激活方向替换为 ReLU6 的候选结构。

3. 实验方案
   - structured_pruning_plan.json：结构化剪枝目标层、比例和约束。
   - activation_replacement_plan.json：SiLU/ReLU6 替换的验证门槛。
   - distillation_plan.json：教师模型、学生模型和蒸馏损失定义。

使用说明
--------

- tools/dataset/distill_desktop6.py 默认使用 yolov8s_pruned10_model.yaml；修改候选结构时，
  应显式传入 --student-config。
- 这些 YAML 只是网络结构定义，不能单独作为板端模型部署。
- 任何候选都必须经过训练、独立 test、ONNX/RKNN 精度对比和 RK3588 实测后，才能称为可用模型。
- 当前生产模型和 4K 专用 INT8 模型不依赖本目录中的候选结构。
