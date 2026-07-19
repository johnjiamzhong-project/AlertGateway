# 4K 检测准确率提升：抽帧、标注、微调与 INT8 校准

## 目标与边界

本流程用于改善真实 4K 场景中 `cup`、`book`（含说明书）和 `laptop` 的漏检、错检及检测框贴合度，
同时保留 `cell phone`、`keyboard`、`mouse` 三个既有类别的能力。

说明书在本轮仍统一标注为 `book`，**不新增类别**。若业务要区分“书”和“说明书”，必须重新定义类别、
补齐两个类别的数据并从头完成训练/评估；不能仅靠量化校准实现。

INT8 校准只用于让已训练模型可靠地转换为 RKNN，不会凭空提高识别率或修正检测框。数据标注与微调
完成、且浮点模型通过独立 4K 测试集后，才进行校准和板端验证。

## 流程

```text
已有 4K 测试视频
  → 每秒抽一帧、按视频片段筛选去重
  → 人工标注 YOLO 框
  → 按视频片段划分 train / val / test
  → WSL/Linux GPU 微调并评估
  → 导出 Rockchip ONNX
  → 选取代表性 4K 帧作 INT8 校准并生成 RKNN
  → RK3588 复播同一测试视频验收
```

训练和转换在主机 WSL/Linux 环境完成；RK3588 只用于最终推理、叠框和性能验证。

## 1. 从已有 4K 视频抽帧

为每段源视频建立独立目录；同一段视频后续只能整体进入 train、val 或 test 其中之一，禁止相邻帧
跨集合混用。先按 1 FPS 抽帧，再人工删除几乎相同、严重模糊、过曝或无法判断的帧。

```bash
mkdir -p /home/rambos/datasets/alertgateway_4k_refine/raw/video_001
ffmpeg -hide_banner -i runs/input_videos/4k/VIDEO.mp4 -vf fps=1 \
  -q:v 2 /home/rambos/datasets/alertgateway_4k_refine/raw/video_001/video_001_%05d.jpg
```

优先保留杯子、说明书/书类、笔记本在以下条件有变化的帧：远近、朝向、开合状态、遮挡、反光、
光照、背景和摆放位置。也要保留少量空桌面、普通纸张、包装盒、屏幕图片等负样本，降低误检。

首次目标建议是筛出约 300--500 张有效 4K 帧，其中三类弱项各至少有 100 个标注实例；样本不足时
再定向补拍视频，而不是先增加量化校准图片。

## 2. 建立标注工作区并标注

目录约定如下。`classes.txt` 的类别顺序必须与现有六类模型一致，不能改动编号。

```text
/home/rambos/datasets/alertgateway_4k_refine/
├── raw/<video_id>/                 # 原始抽帧，永不修改
└── annotation/
    ├── images/                     # 已筛选、待标注图片
    ├── labels/                     # 同名 YOLO 标签
    └── classes.txt
```

```text
0 cell phone
1 cup
2 keyboard
3 mouse
4 laptop
5 book
```

将筛选后的图片复制到 `annotation/images/` 后，创建上述 `classes.txt`，启动项目内置标注器：

```bash
python3 tools/dataset/run_yolo_annotator.py \
  --annotation-dir /home/rambos/datasets/alertgateway_4k_refine/annotation \
  --host 127.0.0.1 --port 8765
```

浏览器打开 `http://127.0.0.1:8765/`。每个检测框应紧贴可见物体边缘：

- 杯子：包含杯身和把手的可见范围；
- 说明书：标为 `book`，包含整本/整张说明书的可见边界；
- 笔记本：包含屏幕和底座的可见整体，屏幕关闭或打开均按同一规则；
- 没有六类目标的有效负样本：使用“标为空负样本”；
- 模糊到无法可靠画框的图片：删除或不要放入 `annotation/images/`。

标注器支持精修：空白处拖动可新建框；点击已有框后，拖动框内部可整体移动，拖动黄色控制点可调整
四角或四边。4K 图像细节不足时点击“放大”，在画面区域滚动查看局部；“适应窗口”恢复默认显示。
完成一张图片后点击“保存”（快捷键 `s`）。

标签格式为每行 `class_id center_x center_y width height`，坐标均相对于图像宽高归一化到 0--1。

## 3. 划分并检查数据集

完成标注后，按整个 `<video_id>` 划分：建议约 70% 视频进 train、15% 进 val、15% 进 test。
测试视频必须完全不参与训练、调参或量化校准。实际目录应满足：

```text
alertgateway_4k_refine/
├── train/images/  train/labels/
├── val/images/    val/labels/
├── test/images/   test/labels/
└── data.yaml
```

`data.yaml` 示例：

```yaml
path: /home/rambos/datasets/alertgateway_4k_refine
train: train/images
val: val/images
test: test/images
names:
  0: cell phone
  1: cup
  2: keyboard
  3: mouse
  4: laptop
  5: book
```

开始训练前应人工复核 test 集，确认三类弱项均有足够实例和场景变化；不能只看总体 mAP，必须查看
每类的 Precision、Recall、AP 以及预测框与人工框的贴合情况。

## 4. 在 WSL/Linux 微调与评估

在带 GPU 的 WSL/Linux Python 环境中进行。项目训练脚本使用已安装的现代 Ultralytics；历史上旧版
定制训练器出现过 NaN 验证损失，不能再作为训练入口。

以下命令以已有 YOLOv8s 权重为起点，50 epoch 只是首轮建议，`batch` 必须按显存调整：

```bash
python3 tools/dataset/train_desktop6.py \
  --weights /home/rambos/yolov8_models/pytorch/yolov8s.pt \
  --data /home/rambos/datasets/alertgateway_4k_refine/data.yaml \
  --epochs 50 --batch 8 --imgsz 640 --device 0 \
  --project /home/rambos/datasets/alertgateway_4k_refine/runs \
  --name yolov8s_4k_refine --export
```

模型输入仍为 640×640；4K 原图会按训练/推理一致的 letterbox 规则缩放。微调依靠人工框计算分类与
定位误差并更新权重，因此能改善框位置；量化校准本身不更新权重。

训练完成后先用从未参与训练的 `test` 集评估，并与当前基线在同一集合、相同阈值下逐类比较。
如果 `cup`、`book`、`laptop` 的 Recall/AP 或框贴合度没有明确提升，不进入 RKNN 转换，应先检查
漏标、类别规则不一致或补充缺失场景。

## 5. INT8 量化校准与 RKNN 转换

仅在浮点模型通过 4K test 集后执行。从 **train/val** 中挑选 150 张代表性 4K 帧作为校准集：应覆盖
目标大小、明暗、背景、杯子/书/笔记本和空桌面；禁止使用 test 图片。无需为校准图片额外标注。

转换脚本要求图片名为 `calib_*.png`。可在独立目录中统一转换后执行：

```bash
python3 tools/convert_int8.py \
  --onnx /path/to/yolov8s_4k_refine_native_640.onnx \
  --calib-dir /home/rambos/datasets/alertgateway_4k_refine/calibration \
  --dataset-txt /home/rambos/datasets/alertgateway_4k_refine/calibration/dataset.txt \
  --dataset-limit 150 --output-layout rockchip_dfl \
  --output /home/rambos/yolov8_models/rknn/yolov8s_4k_refine_int8.rknn
```

转换后必须确认日志没有 float/fp16 fallback，并在相同 4K test 集上比较浮点 ONNX 与 INT8 RKNN 的
逐类结果。只有量化后没有不可接受的精度回退，才允许上传候选模型到板端隔离路径。

## 6. 板端验收与回退

板端用固定的结果输出地址：

```text
rtmp://192.168.0.168/live/alertgateway
```

使用未参与训练的 4K test 视频复播，人工检查三类的漏检、错检和框贴合，并同时记录 FPS、码率、
推理耗时与 `put_fail/out_drop/write_fail`。候选模型、配置与生产模型隔离部署；未完成对照验收前，
不得覆盖当前生产模型或配置。

## 7. 首轮 4K 微调结果（2026-07-17，未部署）

首轮使用常用回放 `VID_20260712_131410` 的 67 张人工标签建立候选：前 54 张训练、后 13 张仅作
同源临时验证；在 GTX 1650 SUPER 4GB 上以 YOLOv8s、640、batch=4、AMP 关闭训练 30 epoch，最佳为
第 25 epoch。该同源验证 mAP50 为 84.45%，只能证明训练链路和候选权重正常，不能作为验收。

独立测试使用未参与训练的 `VID_20260712_131553` 的 63 张人工标签（242 个目标）。候选 `best.pt`
在该测试集的标准结果为：Precision 82.9%、Recall 65.4%、mAP50 72.96%、mAP50-95 61.66%；各类
AP50 为手机 21.07%、杯子 86.72%、键盘 70.06%、鼠标 98.54%、笔记本 82.56%、书本 78.81%。

在相同 `conf=0.30`、NMS IoU=0.45、匹配 IoU=0.50 的固定阈值口径下，当前 ONNX 基线到候选的 Recall
变化为：总体 42.98% → 65.29%，杯子 3.57% → 85.71%，书本 11.94% → 67.16%，鼠标 72.22% →
97.22%；但手机 34.48% → 17.24%，键盘 57.78% → 46.67%，笔记本 89.19% → 75.68%，总体 Precision
从 92.86% 降至 81.03%。

结论：候选明确改善了杯子和书本漏检，但不能因手机和部分原有类别回退而进入 ONNX/RKNN 转换或板端
部署。下一步应从第三段**未用于独立测试**的视频补充训练标签，重点补手机、键盘、笔记本的外观变化，
再与现有 67 张训练数据合并微调，并始终使用本 63 张独立测试集验收。

## 8. 第二轮混合训练结果（2026-07-17，未部署）

第三段 `VID_20260712_131214` 补充标注完成 83 张（1 张空场景），重点增加手机、键盘、鼠标和
笔记本。将历史桌面六类训练集 372 张、首轮 4K 67 张、补充视频前 73 张合并为 512 张训练集；历史
验证 30 张加补充视频后 10 张为临时验证，独立 63 张 test 保持冻结。GTX 1650 SUPER 上训练 20 epoch。

冻结 test 的标准结果为 mAP50 69.83%、mAP50-95 61.71%，AP50：手机 44.98%、杯子 63.81%、键盘
91.79%、鼠标 96.97%、笔记本 80.70%、书本 40.71%。固定阈值口径的 Recall 为：手机 17.24%、
杯子 57.14%、键盘 77.78%、鼠标 97.22%、笔记本 70.27%、书本 16.42%，总体 52.89%。

与首轮 4K-only 候选相比，第二轮显著改善键盘、手机的 AP，但杯子和书本显著回退；说明将大量历史
非 4K 样本等权混入会稀释 4K 场景特征。两轮候选均不满足六类同时维持/提升的门槛，均不得转换或部署。
后续应保持独立 test 不变，在**不查看 test 结果调参**的前提下，以独立 val 选择 4K 样本加权/重复与
历史样本比例，目标是在保住 4K cup/book 的同时恢复 phone/keyboard/laptop。

> 状态更正：上述 63 张集合随后已被用于比较多轮候选并据此改变训练方案，因此从这一刻起只能称为
> `4K dev`，不再是一次性的最终 test。它从未进入训练，但已经参与模型选择。正式部署前必须另建一个
> 从未查看模型结果的新视频级 final test。

## 9. 来源加权与冻结主干候选（2026-07-17，当前 4K 最佳，未部署）

### 数据与选模规则

新增 `prepare_4k_weighted_refine.py`，以硬链接生成可追溯训练集：历史桌面 train 372 张保留一份，
`VID_20260712_131410` 的 67 张和 `VID_20260712_131214` 的 83 张分别重复三份。训练共 822 张，
其中 4K 样本 450 张（54.7%）、历史样本 372 张；实际唯一图像 522 张。组合开发验证为 63 张 4K dev
加 29 张历史回归集，共 92 张；训练与验证图像 SHA-256 重叠为 0。`manifest.csv` 记录每个输出样本的
来源和重复序号。

组合验证的单一 Ultralytics fitness 不能直接作为最终选模依据：以历史六类权重开始完整训练时，
第 16 epoch 的组合 best 在历史域 mAP50 达到 82.25%，但 4K dev 只有 69.90%，杯/书 AP50 仅
70.02%/45.38%。因此后续固定规则为：**4K dev 是主目标，历史域只作不可突破的回归门槛，并分别
报告两个域**。

### 当前 4K 最佳训练方法

最佳候选不从 COCO 或历史六类权重重新开始，而从首轮 4K-only 的 `best.pt` 开始：

- 训练集：上述 54.7% 4K / 45.3% 历史来源加权集；
- 冻结：前 10 个 backbone 模块，只更新检测颈和检测头；
- 优化器：AdamW，`lr0=0.0005`，余弦衰减；
- 输入/批次：640×640、batch 4、AMP 关闭；
- 增强：mosaic 0.5、scale 0.35、translate 0.1、水平翻转 0.5；
- 轮次：2 epoch。继续增加轮次的实验会向历史域偏移并降低 4K cup/book，不采用。

复现命令：

```bash
python3 tools/dataset/prepare_4k_weighted_refine.py

/home/rambos/rknn-env/bin/python tools/dataset/train_4k_weighted_refine.py \
  --weights /home/rambos/datasets/alertgateway_4k_refine/runs/yolov8s_4k_refine_30e/weights/best.pt \
  --epochs 2 --freeze 10 --lr0 0.0005 \
  --name yolov8s_4k_first_head_weighted_2e_20260717
```

当前权重：

```text
/home/rambos/datasets/alertgateway_4k_refine/runs/
  yolov8s_4k_first_head_weighted_2e_20260717/weights/best.pt
```

### 结果

在 63 张 4K dev 上，当前候选 mAP50 为 79.91%、mAP50-95 为 70.95%；逐类 AP50：手机 50.52%、
杯子 85.56%、键盘 86.09%、鼠标 99.39%、笔记本 81.93%、书本 75.98%。固定 `conf=0.30`、
NMS IoU=0.45、匹配 IoU=0.50 时，总体 Precision/Recall/F1 为 86.21%/72.31%/78.65%，匹配框平均
IoU 为 92.68%；逐类 Recall 为手机 41.38%、杯子 78.57%、键盘 86.67%、鼠标 100%、笔记本
78.38%、书本 55.22%。

相对首轮 4K-only 候选，当前候选总体 Recall 从 65.29% 提升到 72.31%，手机、键盘、笔记本分别从
17.24%/46.67%/75.68% 提升到 41.38%/86.67%/78.38%；杯子和书本从 85.71%/67.16% 回落到
78.57%/55.22%，但仍远高于原始 ONNX 基线的 3.57%/11.94%。这是当前已测候选中最均衡的 4K 结果。

历史 29 张回归集的 mAP50/mAP50-95 为 65.26%/53.03%，低于历史六类基线 77.18%，主要回退在
历史 mouse/laptop。因此该权重只能作为 **4K 专用浮点测试候选**，不能替换通用生产模型，也不得
据此导出/部署 RKNN。

### 下一步门槛

1. 从未用于训练和模型选择的新视频建立 final test；现有 63 张继续只作 dev。
2. 若业务只切换 4K 拉流模型，final test 一次性通过后再导出 ONNX、INT8 RKNN，并隔离上板 A/B。
3. 若 V4L2/历史桌面与 4K 必须共用一个模型，先补充历史 mouse/laptop 和新 4K laptop 场景，重新
   做来源/类别采样；历史回归 mAP50 至少恢复到 72.18% 后才允许进入转换。
4. 未建立 final test 前，不再根据当前 63 张继续试比例或轮次，避免进一步把 dev 调成训练目标。
