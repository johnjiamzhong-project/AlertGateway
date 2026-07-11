# AlertGateway 项目记忆

最后更新：2026-07-11

## 使用规则

每次开始处理本项目之前，必须先完整阅读本文件，再检查与当前任务直接相关的源码、
文档和 Git 状态。后续工作应基于这里记录的已确认状态增量推进，不要脱离现状重新设计
整个项目。

本文件记录项目事实、已作出的决定和下一步方向。完成重要实验、改变技术路线或发现原有
记录不再准确时，应同步更新本文件；详细过程仍写入对应的 `docs/` 文档，不在这里重复堆积。

## 项目定位

AlertGateway 是运行在 Firefly ROC-RK3588S-PC 上的桌面物品检测验证项目，主要业务链路为：

```text
V4L2摄像头采集
├── InferThread：RGA预处理 → RKNN YOLOv8s INT8推理 → 检测结果
└── EncodeThread：RGA YUYV→NV12 → 叠加检测框 → MPP H.264编码

检测结果 → SharedDetections → EncodeThread
编码数据 → StreamThread → FFmpeg/RTMP
检测摘要 → MqttThread → Paho MQTT
```

项目是面向RK3588硬件的C++17交叉编译工程，不是当前x86主机可直接完整运行的普通应用。

## 当前已确认状态

- Git分支：`master`，当前领先 `origin/master` 4 个提交；具体 HEAD 以 `git log -1` 为准。
- 工作区存在用户自己的未提交修改和未跟踪文档，处理任务时必须保留，不能覆盖或清理。
- 本地已有ARM64构建产物 `build/AlertGateway`。
- 仓库内 `model/` 只有 `.gitkeep`；实际运行所需的
  `model/yolov8s_rockchip_dfl.rknn` 未纳入Git。
- 运行配置为摄像头640×480、15 FPS、模型输入在当前代码中固定为640×640。
- 当前生产配置已切换为 Rockchip 官方九输出 YOLOv8s INT8，使用 rockchip_dfl 后处理；旧 decoded 双输出模型仍作为对照基线。
- 推理和编码已经解耦；推理结果通过 `SharedDetections` 提供给编码线程。
- 推理和编码预处理均优先使用RGA，失败时保留CPU兜底。
- 视频流叠框已支持在检测框顶部/内部绘制“类别 + 置信度”的中文文本标签（如“杯子 87%”），基于内置的 8x16 ASCII 及 16x16 中文位图字体，在 NV12 DRM Buffer 上直接渲染，支持 UTF-8 字符解码与动态宽度计算，不引入外部 OpenCV 依赖，并作了严格裁剪防御越界。
- 中文类别名仅在编码绘制层映射，内部 `Detection.label` 和 MQTT `objects[].label`
  保持英文 COCO 类别名；`stream.draw_detection_labels` 可关闭文字标签，
  `stream.bitrate_kbps` 可配置 MPP CBR 目标码率，旧配置分别默认 `true` 和 `2000`。
- 项目目前没有自动化单元测试或集成测试。

## 当前性能基线

- EXP-001已完成输入API、输入内容、调用节拍、输出模式、perf查询和CPU调度的逐项对照。
- EXP-002已完成当前双输出模型`optimization_level=0/1/2/3`单变量对比；CPU6固定口径下
  中位数分别为29.249/29.240/29.239/29.237 ms，最大差约0.04%，没有实际性能收益。
- 真实AlertGateway采集1245帧：
  `rknn_run mean=29.868 ms, median=29.429 ms, P90=30.573 ms`，旧29.4 ms基线有效。
- 独立benchmark未绑定CPU时约30.4 ms；固定到CPU6大核后，当前模型为
  `median=29.237 ms, P90=29.249 ms`。此前约1 ms差异来自CPU核心调度，不是模型退化。
- Rockchip官方优化检测头INT8模型在相同CPU6、零拷贝、固定输入和锁频条件下：
  `median=25.662 ms, P90=25.675 ms`，NPU阶段比当前模型快约12.2%。
- EXP-003已完成官方9输出模型的INT8筛选、CPU端DFL、坐标解码和NMS。固定bus图片上，
  两种模型均检出相同的4个人和1辆公交车，框位置接近。
- CPU6固定口径下，当前模型“run+outputs_get+完整后处理”中位数为30.540 ms，官方模型
  为27.746 ms；官方完整链路净快2.794 ms，约9.15%。
- 官方模型已具备进入生产接入验证的性能依据，但固定图片检查不能替代标注集精度评估，
  目前不能直接替换生产模型。
- EXP-004已将官方九输出路径接入生产`InferThread`，通过`model.output_layout`在
  `decoded`和`rockchip_dfl`之间显式切换；字段缺失时默认`decoded`。
- 抽取后的生产后处理组件通过固定bus图片回归；真实摄像头两种路径各运行60秒/870次推理
  零错误，官方路径另完成300秒/4476次推理稳定性测试，RSS稳定在约31.2～31.6 MB。
- SRS恢复后，EXP-004已完成120秒完整流水线验证：MQTT连接成功、SRS确认
  `/live/desk`发布活跃且为H.264 Main 640×480、编码稳定在15.1 FPS，1765次推理无错误并
  正常清理退出；另从SRS成功抓取并解码一帧。当前摄像头画面模糊且没有明确的六类目标，
  尚未完成检测框人工对照。
- 2026-07-06完成文字标签画质初步A/B：相同机位分别抓取约9.6秒视频，配置2000和
  3000 kbps时实测平均码率为2.010和3.014 Mbps，码率配置生效；抓帧中3 Mbps细小边缘
  略完整，但屏幕内容发生变化，且摄像头源画面存在明显泛白、反光/失焦。测试期间检测
  持续为0个目标，未实际绘制文字，故尚不能量化文字标签本身的画质影响。板端已部署
  `draw_detection_labels=true`、`bitrate_kbps=3000`候选版本并已启动，
  `rockchip-performance`锁频服务处于active状态；SRS流验证为H.264 Main、
  640×480、约15.17 FPS。
- 2026-07-07 完成固定桌面场景（带 COCO 六类目标）的文字标签及 2/3 Mbps A/B 验证（EXP-005）。测试配置 `output_layout=decoded`，通过板端本地 FLV 写入规避 SRS 网络连通限制，每组运行约 25 秒。实际测量码率：3M 组为 2992/2986 kbps（偏差 <0.4%），2M 组为 1984/1989 kbps（偏差 <0.8%）；ffprobe 帧率稳定在 15.17 fps，日志平均编码帧率 13.4 fps；文字标签开关及码率变化对系统帧率与推理效率无负面影响；全过程稳定检测到目标，零错误，板端配置及进程均已恢复。
- 2026-07-08 完成 EXP-005 人工画质评估与 30 帧人工精度核对。黑底中文标签清晰度与遮挡均在极佳状态；2 Mbps 码率细节虽微有蚊噪但完全可接受并节省 33.3% 带宽，推荐作为生产配置。30 帧核对发现戴尔显示屏稳定检出但错检为 laptop，易拉罐全程漏检，棉柔巾纸盒存在低置信度误检，建议设置过滤阈值 conf_threshold >= 0.25。结论允许且推荐进入固定标注集自动评估阶段。
- 2026-07-08 完成 EXP-006 官方九输出与生产双输出模型精度量化仿真对比。两者整体 mAP 均完全对齐（COCO 子集 60.16%、桌面场景 48.33%），证明精度无任何损失，且九输出在部分类别（cup/laptop）的 Precision 指标上因精细 Softmax 而略微领先。已正式将生产配置 config.json 中的 model.output_layout 从 decoded 切换为 rockchip_dfl。
- 2026-07-08 完成板端生产配置 Smoke 真机验证。部署新命名的官方九输出模型 ~/AlertGateway/model/yolov8s_rockchip_dfl.rknn，并将 config.json 的 model.output_layout 切换为 rockchip_dfl，conf_threshold 设定为 0.25，stream.bitrate_kbps 设定为 2000。程序在本地 FLV 推流配置下稳定运行 62.533 秒，无任何推理/编码链报错，并在超时后通过 SIGTERM 信号优雅退出（Done），证实了九输出模型固件与生产配置的配置匹配正常。
- 2026-07-08 已开始 640x480 输入尺寸优化准备：`tools/collect_calibration.py` 支持通过
  `--model-width/--model-height` 采集不同模型输入尺寸的校准图；`tools/convert_int8.py`
  支持参数化 ONNX、校准目录、dataset.txt、输出路径和 `decoded/rockchip_dfl` 输出布局；
  `tools/benchmark/evaluate_precision.py` 支持通过 `--model-width/--model-height` 按模型
  输入尺寸做 resize、坐标回映射和 rockchip_dfl 输出网格校验，并可通过 `--model-mode`
  单独评估某一种输出布局。
- 2026-07-08 完成首个 640x480 九输出候选验证：基于现有 640x640 官方九输出 ONNX 修改
  静态输入/输出形状生成 `/home/rambos/yolov8_models/onnx/yolov8s_official_640x480.onnx`，ONNX Runtime
  验证输出网格为 60x80、30x40、15x20；使用从板端拉回的 150 张 640x480 校准图成功
  转换 `/home/rambos/yolov8_models/rknn/yolov8s_rockchip_dfl_640x480.rknn`，大小 12M，转换脚本检查为
  全 INT8、无 float/fp16 fallback。Simulator 精度评估显示 COCO 子集 mAP 从 60.16%
  升至 68.15%，但桌面场景 mAP 从 48.33% 严重降至 8.33%，laptop Recall 从 96.67%
  降至 16.67%。该候选不能进入生产 C++ 尺寸放开和板端 smoke。
- 2026-07-08 复测原生导出的 640x480 官方九输出 ONNX：
  `/home/rambos/yolov8_models/onnx/yolov8s_official_native_640x480.onnx` 由 `ultralytics_yolov8` 定制库
  `rk_opt_v1.6` 分支导出，文件 hash、权重 initializer 和转换得到的 RKNN hash 均不同于
  改形候选；转换 `/home/rambos/yolov8_models/rknn/yolov8s_rockchip_dfl_native_640x480.rknn` 成功，大小
  12M，转换脚本检查为全 INT8、无 float/fp16 fallback。但 Simulator 精度结果与改形候选
  完全一致：COCO 子集 mAP 68.15%，桌面场景 mAP 8.33%，laptop Recall 16.67%，cup
  Recall 0.00%。因此当前所有 640x480 候选均不允许进入生产。
- 2026-07-08 完成 576x448 与 512x384 输入尺寸候选验证。两个尺寸均由
  `ultralytics_yolov8` 定制库 `rk_opt_v1.6` 原生导出官方九输出 ONNX，并成功转换为
  全 INT8 RKNN：`/home/rambos/yolov8_models/rknn/yolov8s_rockchip_dfl_native_576x448.rknn` 与
  `/home/rambos/yolov8_models/rknn/yolov8s_rockchip_dfl_native_512x384.rknn`。Simulator 精度评估显示
  576x448 的 COCO 子集 mAP 为 61.39%，但桌面场景 mAP 仅 13.33%，laptop Recall
  26.67%；512x384 的 COCO 子集 mAP 为 53.61%，桌面场景 mAP 仅 5.00%，laptop
  Recall 10.00%。两者均低于 640x640 `rockchip_dfl` 桌面基线 48.33%/96.67%，不能
  进入生产。
- 2026-07-08 已整理输入尺寸实验收尾：根目录临时 `check*.onnx` 中间模型移出仓库到
  `/tmp/alertgateway_onnx_artifacts/`，`.gitignore` 已忽略根目录生成的 `.onnx/.rknn`；
  `tools/export_native_640x480.py` 和 `tools/export_native_candidates.py` 已整理为可复用
  参数化导出脚本；`TODO.md` 已移除完成项，仅保留后续未做工作。
- 2026-07-09 已同步 `readme.md` 与当前实现：生产模型路径、`rockchip_dfl` 输出布局、
  15 FPS 摄像头配置、文字标签、2000 kbps 码率配置、实际构建命令和文档入口均已对齐。
- 2026-07-09 已整理 CMake 交叉编译参数：`ALERTGATEWAY_SYSROOT` 可覆盖默认
  `/home/rambos/sysroot`，`RKNN_LIB_PATH` 现在实际决定 RKNN 交叉链接 stub，且兼容传入
  `librknnrt.so` 文件路径或其所在目录。
- EXP-001至EXP-008详细操作与结果见
  `docs/YOLOv8s-RK3588推理优化实验记录.md`。
- 测试时已经关注CPU/NPU governor；CPU频率会明显影响 `rknn_run()` 的同步耗时。
- 当前摄像头可以任意朝向用于测量纯 `rknn_run()`，因为固定输入尺寸下的稠密卷积计算量
  基本不随画面内容变化。
- 正式比较模型版本时，应改用同一个固定输入，预热100次、测试至少1000次，并记录中位数
  和P90。
- Rockchip Model Zoo当前公开的RK3588 YOLOv8s INT8数据为40.8 FPS，折算约24.5 ms，
  且标注为单核口径。网上约20 ms的数据不能在模型图、输入尺寸、工具链和测试环境不同的
  情况下直接比较。
- 2026-07-09 已新增桌面六类数据集与微调方案文档 `docs/YOLOv8s-RK3588桌面六类数据集与微调方案.md`，并补充了实际采集与整理命令模板。
- 2026-07-09 已编写并测试轻量脚本 `tools/dataset/prepare_desktop6_dirs.py`，并在仓库外路径 `/home/rambos/datasets/alertgateway_desktop6` 初始化了数据集目录及 classes.txt/data.yaml 模板，当前进入并完成了“采集目录与命令模板准备”阶段。
- 2026-07-09 已新增脚本 `tools/dataset/collect_desktop6_checklist.sh`，用于打印桌面六类采集与整理的完整步骤清单，并已在文档中进行引用。
- 2026-07-09 已完成首批 Win11 摄像头桌面六类视频采集并复制到
  `/home/rambos/datasets/videos/`。除预览 FLV 外，已保留 `collect_001` 至 `collect_011`
  共 11 段 MP4，覆盖全物品、键盘/鼠标/手机、杯/书/手机、键盘/鼠标/杯、显示器、打开
  笔记本、杯类专项、书本专项、杂乱负样本和空桌面负样本。抽帧检查已完成：`collect_002`
  至 `collect_004` 按 1 FPS 抽出 73/87/105 张；`collect_005` 至 `collect_011` 按 2 秒
  间隔抽出 38/17/49/71/50/33/25 张。总体质量可用；`collect_001` 只建议少量保留，
  `collect_010` 需剔除含六类目标或过糊帧，`collect_011` 是理想空桌面负样本。
- 2026-07-09 已新增 `tools/dataset/prepare_review_manifest.py`，并生成首批筛图工作台：
  `/home/rambos/datasets/selected_desktop6/review/review_manifest.csv` 和
  `/home/rambos/datasets/selected_desktop6/review/review_index.html`，共索引 626 张候选帧。
- 2026-07-09 已新增 `tools/dataset/apply_review_selection.py`，用于读取
  `review_manifest.csv` 的 `keep` 标记，并将选中帧复制到
  `/home/rambos/datasets/selected_desktop6/images/`；脚本不会删除或移动原始抽帧，并支持用
  `--exclude-session` 排除已知不应进入训练集的采集批次。
- 2026-07-09 已增强 `tools/dataset/prepare_review_manifest.py`：重新生成 review 工作台时会将
  原始抽帧复制到 review 目录下的 `_frames/`，HTML 使用相对路径加载图片，并支持在页面中
  按“默认全部保留、只删除坏帧”的逻辑筛图、隐藏已删除图片并导出保留 CSV。
- 2026-07-09 用户已从 review 页面导出 `/home/rambos/datasets/review_keep_selection.csv`。
  经 `tools/dataset/apply_review_selection.py --exclude-session collect_preview_001` 处理，已将
  553 张筛选保留图片复制到 `/home/rambos/datasets/selected_desktop6/images/`；CSV 中另有
  5 张 `collect_preview_001` 预览帧被排除，selected images 目录中 preview 图片数量为 0。
- 2026-07-09 已新增并运行 `tools/dataset/prepare_annotation_workspace.py`，在
  `/home/rambos/datasets/alertgateway_desktop6/annotation/` 生成标注工作区：`images/`
  中 553 张图片，`classes.txt` 为 6 类 COCO 名称，`annotation_manifest.csv` 记录图片来源、
  session、目标 label 路径和 `negative_hint`。其中负样本提示共 53 张（`collect_010` 和
  `collect_011`）；`labels/` 目前保持为空，避免未标注正样本被误当空标签训练。
- 2026-07-09 已新增 `tools/dataset/run_yolo_annotator.py`，这是一个仅依赖 Python 标准库的
  本地浏览器 YOLO 标注工具。当前已启动在 `0.0.0.0:8765`，页面/API 已验证可返回 553 张
  annotation 图片和 6 类类别；保存时会直接写入
  `/home/rambos/datasets/alertgateway_desktop6/annotation/labels/`。
- 2026-07-09 已增强本地标注工具交互：选中已有框后，点击类别按钮或按数字键 0-5 会修改
  该框类别；未选中框时类别按钮只设置下一次新画框的默认类别。
- 2026-07-09 已修正本地标注工具画框起点冲突：鼠标按下后拖动超过阈值即开始新画框，
  只有单击已有框时才选中该框，因此新框可以从已有框内部或边缘起笔。
- 2026-07-09 已修正本地标注工具贴边标注：拖框时鼠标可移出图片区域，释放时坐标会自动
  clamp 到图片边界，便于标注主体明确但贴边的目标。
- 2026-07-09 已为本地标注工具增加“删除图片”能力：当前图片可一键移出 active 标注集，
  图片会被移动到 `annotation/deleted/images/`，对应 label（若存在）会同步移动到
  `annotation/deleted/labels/`，便于跳过数量过多且无需标注的样本。
- 2026-07-09 已为本地标注工具增加按序号跳转能力：页面支持直接输入第 N 张并跳转，
  也支持回车触发，便于在大量图片中快速定位和复核。
- 2026-07-09 用户已完成 188 张图片的人工标注，现决定暂停继续逐张标注；后续应优先评估
  这批现有标签是否足以进行下一阶段训练或验证，再决定是否补标。
- 2026-07-09 已自动补齐 `negative_hint=1` 的可确认负样本空 label：共创建 48 个空标签文件，
  `annotation/labels` 现有 533 个标签文件；另导出 20 张仍需人工复核的未标注正样本到
  `annotation/pending_manual_review.csv`。
- 2026-07-09 已按 session 将 533 张可用样本切分到
  `/home/rambos/datasets/alertgateway_desktop6/train|val|test/`，采用硬链接方式避免重复占用
  磁盘；当前划分为 train 435、val 45、test 53，20 张 pending review 未纳入切分。
- 2026-07-09 已新增并运行 `tools/dataset/pseudo_label_desktop6.py`。主机 RKNN Toolkit
  不能直接用 `load_rknn` 在 simulator 推理板端 RKNN，因此脚本默认使用 ONNX Runtime 读取
  `/home/rambos/yolov8_models/onnx/yolov8s_official.onnx` 的九输出 `rockchip_dfl` 张量生成草稿 YOLO 标签。
  已批量预标注 annotation 图片：`labels/` 现有 481 个 `.txt` 文件、0 个空标签文件、共
  1175 行框；另有 72 张图片模型未检出目标，保持未写 label，避免将漏检图误当负样本。
  预标注 manifest 位于
  `/home/rambos/datasets/alertgateway_desktop6/annotation/pseudo_label_manifest.csv`。
- 2026-07-09 完成了训练前数据完整性与统计分析。验证了 train/val/test 的 session 隔离，无 session 泄露；当前 6 类别的框分布比例（最大 cup 占比 26.26%，最小 laptop 占比 7.84%，极值比 3.35x）处于中等不平衡水平，可直接训练；20 张 pending manual review 图片已确认其 session 分布（17 张在 train，3 张在 val），微调开始前不需要提前补标，可在微调后根据需要增量补标。
- 2026-07-09 新增了 `tools/dataset/check_dataset_stats.py`（数据集统计分析工具）和 `tools/dataset/train_desktop6.py`（YOLOv8s 训练及原生九输出 ONNX 自动导出脚本）。
- 2026-07-09 用户随后报告：30 轮小规模试训、9 输出 ONNX / 100% INT8 RKNN 导出、评估脚本越界修复、完整精度分析与项目归档已经完成；但当前工作区尚未找到 `walkthrough.md`，且未在本地再次核实对应 runs/ 报表和模型产物，因此这些“已完成”内容暂记为待核实报告，不写成已验证事实。
- 2026-07-10 已完成并核实桌面六类 30 epoch 微调。仓库内原有 `runs/detect/train` 实际仅为 2 epoch 试跑，未找到此前报告的 30 epoch 训练、微调 ONNX/RKNN、精度报告或 `walkthrough.md`。首次使用定制 `ultralytics_yolov8` 8.0.151 训练时，验证损失从首轮起即为 NaN，最终 mAP@0.5 仅 0.39%；该权重及其九输出 ONNX 不允许继续转换。随后使用 `/home/rambos/rknn-env` 中 Ultralytics 8.4.67、CUDA、AMP 关闭、batch 8、640x640 完成有效 30 epoch 训练，产物位于 `/home/rambos/datasets/alertgateway_desktop6/runs/yolov8s_desktop6_30e_20260710_v2/`。最佳为第 2 epoch，best.pt 的 val Precision/Recall/mAP@0.5/mAP@0.5:0.95 为 53.0%/33.7%/34.49%/25.01%，低于生产 48.33% mAP@0.5 门禁，因此暂停 ONNX/RKNN 和板端验证。当前 val 仅一个 session，cell phone/cup/laptop 各 2 个实例；test 仅 5 个正框，评估集代表性不足。
- 2026-07-10 已完成人工复核 `pending_manual_review.csv` 的 20 张图片：11 张保存有效人工标签，9 张模糊/不可用图片移入 `annotation/deleted/`；11 张新增框为 cell phone 1、cup 21、laptop 1、book 9，标签格式与坐标范围检查零错误。active 标注集现为 544 张图片/544 份标签，原 pending 清单已归档为 `pending_manual_review.completed_20260710.csv`。按 session 统计后确认：现有数据只有 `collect_001` 和 `collect_003` 同时覆盖六类，直接重划为 val/test 会明显削弱训练集；下一步改为新采两个相互独立、均覆盖六类的专用 val/test session，并全部人工标注。
- 2026-07-10 复核 session 组合后取消继续采集视频：现有数据可用 `collect_001` + `collect_006` 组成 val、`collect_003` + `collect_005` 组成 test，其余 session 作为 train 候选。新增 `tools/dataset/prepare_eval_review_workspace.py`，已在 `/home/rambos/datasets/alertgateway_desktop6/eval_review/` 生成隔离复核工作区，确定性稀疏选择 val 47 张/test 60 张并复制现有候选框；val 六类框数为 11/44/22/15/24/23，test 为 25/72/49/39/22/25。原 active 图片和标签未修改，人工复核完成后再合并评估标签并生成最终划分。
- 2026-07-10 根据人工工作量反馈，将评估复核从 107 张缩减为固定 60 张，页面序号范围为 1～20、31～50、61～70、91～100；候选框六类统计为 25/84/37/26/25/33。未修改的候选框可直接进入下一张，只有修正框时才点击保存，坏图仍使用删除；未选中的评估工作区图片不进入最终 val/test。
- 2026-07-10 已将 `/home/rambos` 顶层的 17 个 YOLOv8 模型文件按格式归档到
  `/home/rambos/yolov8_models/pytorch|onnx|rknn/`，未删除或改名；仍在使用这些模型的工具
  默认路径已同步更新。仓库内和 `runs/` 内的模型文件及历史实验记录未移动。

## 2026-07-10 桌面六类微调与量化暂停点

- 固定 60 张评估候选已复核：修改 30 份标签、删除 3 张坏图。最终数据集为 `alertgateway_desktop6_final`，train/val/test=372/30/29 张、746/87/130 框，按 session 隔离且无泄漏。
- Ultralytics 8.4.67、640x640、batch 8、AMP 关闭的有效 30 epoch 已完成；best.pt 的 val mAP@0.5=58.0%，独立 test mAP@0.5=77.2%，超过 48.33% 门槛。
- 已生成九输出 ONNX `yolov8s_desktop6_final_native_640.onnx`、桌面校准 INT8 RKNN `yolov8s_desktop6_final_int8_desktop_calib.rknn` 和 150 张校准集 `calibration_640`。
- 最终 test 在 conf=0.25/NMS=0.45 下：ONNX letterbox/拉伸=53.96%/42.62%，RKNN Simulator=56.08%/39.04%。未发现 INT8 量化损失；生产端直接拉伸是主要问题，拉伸时 laptop AP 为 0%。旧 20 COCO+30 桌面固定集的 8.33%/0% 与新六类模型不匹配，不再作为部署门槛。
- 生产 C++ 工作区已支持 Rockchip 九输出 80/6 类，并在 Rockchip 路径使用 114 灰边 letterbox、去 padding 坐标回映及同几何 CPU 兜底；三个构建目标交叉编译通过，但尚未上板 smoke、尚未部署，板端现有生产配置不变。
- 暂停时没有遗留训练、量化或评估进程；未执行 commit/push。

## 已确定的优化原则

优化按照从简单到困难的顺序进行：

1. 统一官方模型与当前模型的benchmark口径；
2. 对比RKNN `optimization_level=0/1/2/3`；
3. 采用Rockchip官方优化检测头，将DFL和部分后处理移出NPU；
4. 将模型输入从640×640改为匹配摄像头比例的640×480；
5. 再评估576×448和512×384；
6. 建立桌面六类业务数据集并微调；
7. 评估介于YOLOv8n和YOLOv8s之间的自定义宽度模型；
8. 最后再考虑检测尺度调整、结构化剪枝、模块替换和知识蒸馏。

暂不把校准算法、混合精度和QAT作为降低同一INT8计算图执行时间的首选方案；它们主要用于
解决量化精度问题。

完整路线见：

- `docs/YOLOv8s-RK3588推理性能优化路线.md`

## 精度验证决定

- 前期探索可以人工查看20～50张图片，快速排除严重错误。
- 正式保留一个优化方案前，需要用固定标注集自动评估。
- 人工只需为测试集制作一次Ground Truth，之后自动计算Precision、Recall、mAP、漏检和误检。
- 验证集应同时包含COCO六类子集和真实桌面场景数据。
- 所有模型必须使用相同测试集、置信度阈值、NMS参数和预处理方式进行比较。

## 文档组织决定

- `docs/YOLOv8s-RK3588推理性能优化路线.md`：保存稳定的目标、路线和实施顺序。
- 后续应建立 `docs/YOLOv8s-RK3588推理优化实验记录.md`：按实验编号持续记录行为、
  预期、环境、实际结果、精度、原因分析、结论和后续行动。
- 失败实验也要记录，避免以后重复执行。
- 只有实验结论改变整体方向时，才同步修改路线文档。
- 2026-07-07 已清理 `TODO.md`：移除已完成/过期的旧量化实验表述，将当前主线对齐为
  固定桌面场景人工对照、文字标签画质 A/B、固定标注集评估和生产切换决策。
- 2026-07-07 已修正 MQTT 去重逻辑：保留上报 payload 中的 `timestamp`，但使用不含
  timestamp 的 `objects` 内容签名做变化判断，避免时间变化导致每个周期重复上报。
- 2026-07-07 已补充 V4L2 协商参数校验：`CaptureThread` 在 `VIDIOC_S_FMT/S_PARM`
  后回读并记录实际 width、height、pixelformat、bytesperline、sizeimage 和 fps；
  非 YUYV、分辨率不匹配或 YUYV 行跨度不匹配会启动失败，帧率偏差会告警。
- 2026-07-07 已补充关键调用返回值检查：MPP 初始化配置、DRM buffer 申请、
  MppFrame 初始化、`encode_put_frame`、编码包 metadata/空包、FFmpeg 网络初始化、
  extradata 分配和 AVPacket 分配失败路径均已记录日志并中止启动或跳过当前帧。
- 2026-07-07 板端 smoke 验证通过：仅部署新二进制，保留板端真实配置；因
  `192.168.0.168:1935` 的 SRS 未启动，使用板端临时 FFmpeg RTMP listener 和临时配置
  `rtmp://127.0.0.1/live/desk` 跑满 60 秒。V4L2 协商为 640×480 YUYV、
  `bytesperline=1280`、`sizeimage=614400`、15/1 fps；MPP 按 640×480、15 fps、
  3000 kbps CBR 初始化；编码稳定打印 15.1 fps；FFmpeg 成功识别并解码 H.264 Main
  yuv420p 640×480、约 15.17 fps，累计接收 860 帧。`ALERTGATEWAY_RC=124` 来自
  `timeout` 主动中断，日志正常 `Shutting down... Done.`，未触发新增错误路径。

## 已发现但尚未处理的工程问题

这些问题不是当前算法优化主线，但后续不能遗忘：

- 交叉编译工具链文件实际名称是`cmake/aarch64-toolchain.cmake`；旧文档中的
  `cmake/aarch64-linux-gnu.cmake`不存在，已修正开发指南。

## 历史下一步（已过时）

- 使用 `/home/rambos/datasets/alertgateway_desktop6/eval_review/` 复核固定 60 张候选评估图片（序号 1～20、31～50、61～70、91～100）；只保存实际修改，复核完成后生成最终 val/test，并确保同 session 的未选图片不进入 train。
- 修正 `tools/dataset/train_desktop6.py`：训练阶段不得强制加载旧定制 Ultralytics 8.0.151；新版训练与 Rockchip 九输出导出应使用隔离进程或环境，避免旧训练器的 NaN 验证问题。
- 用相同 Ultralytics 8.4.67、640x640、batch 8、AMP 关闭口径重新训练和评估；达到或超过 48.33% mAP@0.5 基线后，才导出定制九输出 ONNX、转换全 INT8 RKNN 并做板端 Smoke。

## 当前暂停状态与下一步（以本节为准）

- 当前按用户要求暂停，不进行板端部署。
- 恢复后第一优先级：以候选文件名上传新二进制和 `yolov8s_desktop6_final_int8_desktop_calib.rknn`，用临时配置运行 `infer_camera_smoke`/短时 `AlertGateway`；验证 6 类输出校验、RGA letterbox、坐标叠框、稳定性和耗时。不覆盖现有生产模型与配置，通过后再决定是否切换。
- 修正 `tools/dataset/train_desktop6.py`，禁止训练阶段加载旧定制 Ultralytics 8.0.151；训练和 Rockchip 九输出导出使用隔离环境/进程。
- 新六类 YOLOv8s 板端基线固定后，再做 0.375 宽度自定义模型；随后依次评估检测尺度调整、结构化剪枝、模块替换和知识蒸馏，每一步沿用同一 final val/test、letterbox、阈值和 RKNN 板端口径。

## 2026-07-10 候选板端 Smoke 与训练入口

- 板端 `192.168.0.200` 可达；候选文件已上传到隔离目录 `~/AlertGateway/candidate_20260710/`，未覆盖生产目录。
- 候选 RKNN 成功完成 `rknn_init`、9 输出布局校验和 zero-copy 输入绑定；失败发生在采集阶段：候选配置使用的 `/dev/video20` 不存在。
- 板端当前 ISP 节点为 `/dev/video11` 等，主要支持 UYVY/NV12，不支持现有 CaptureThread 要求的 YUYV；因此暂不擅自修改采集格式链路，Smoke 状态记为“模型加载通过、摄像头采集阻塞”。
- `tools/dataset/train_desktop6.py` 已修复：训练默认使用现代已安装 Ultralytics，拒绝 `--ultralytics-dir` 旧路径；Rockchip 导出改为可选独立进程。已通过 `py_compile` 和 `--help` 验证。


## 2026-07-10 摄像头格式适配小任务

- 已确认最小适配路径为：板端多平面 UYVY 采集，在 CaptureThread 边界转换为内部 YUYV；不改 Frame、EncodeThread 和 InferThread 的内部数据接口。
- 已实现多平面 V4L2 格式协商、mmap、DQBUF/QBUF、UYVY→YUYV 转换及对应 STREAMON/STREAMOFF 路径；完整交叉编译通过。
- 候选板端验证：/dev/video11 成功协商 640x480 UYVY、bytesperline=1280；QBUF 通过，但 STREAMON 返回 Operation not permitted。独立 v4l2-ctl 也复现同样错误，media-ctl 显示 ISP 传感器输入链接未启用，故当前阻塞来自板端摄像头源状态，不是模型或格式转换代码。

## 2026-07-10 ISP 拓扑调查

- 只读检查 /dev/media0 显示 CSI2/DPHY 与 rkcif 节点存在，但媒体拓扑中没有实际摄像头 sensor 实体；因此 /dev/video11 的 STREAMON=EPERM 属于板端摄像头未识别/未接入状态，不是 CaptureThread UYVY 适配错误。


## 2026-07-10 0.375 宽度配置准备

- 新增 	ools/dataset/yolov8d375_desktop6.yaml：六类检测、深度 0.33、宽度 0.375、最大通道 1024。
- 已用当前 Ultralytics 8.4.67 独立构建并加载现有 best.pt，验证通过（Transferred 95/355 items）。
- 原 yolov8s_desktop6_w0375.yaml 会被文件名中的 s 自动推断为标准 s 尺度，暂不作为训练入口；后续使用 d375 文件。
- 下一小步：仅做该模型的短跑微调/评估，不与板端摄像头联调并行。


## 2026-07-10 0.375 short run

- 5 epoch smoke training completed: yolov8d375_desktop6_smoke_5e_20260710.
- 6,420,250 params, 16.8 GFLOPs, weight transfer 355/355.
- val mAP50=16.6%, mAP50-95=7.13%; this only validates the training path.
- Next: 30 epoch fine-tuning and independent test; do not export RKNN yet.


## 2026-07-10 0.375 完整评估结论

- 30 epoch 微调完成，val mAP50=54.3%。
- 固定 test mAP50=42.3%、mAP50-95=28.1%，显著低于六类 YOLOv8s 基线 test mAP50=77.2%。
- 结论：0.375 候选淘汰，不进入剪枝/模块替换/蒸馏；后续结构优化回到基线模型。


## 2026-07-10 最新状态同步

- 0.375 宽度模型已完成 30 epoch 和独立 test，test mAP50=42.3%，低于六类 YOLOv8s 基线 77.2%，已淘汰。
- 当前后续优化起点固定为六类 YOLOv8s 基线；先做 RKNN profiling，再决定算子融合、通道对齐、结构化剪枝和模块替换。
- 蒸馏仅用于结构改动后的精度恢复，不直接作为 NPU 加速手段。
- 板端完整摄像头 Smoke 仍受 ISP sensor 输入链路阻塞，暂不重复采集视频。


## 2026-07-10 基线 RKNN profiling

- 使用隔离候选目录和六类 9 输出 RKNN，在 RK3588 上 warmup=50、runs=300、zero-copy、auto core 完成 profiling。
- rknn_run 平均 50.707 ms，P90 53.032 ms；outputs_get 平均 4.276 ms；后处理平均 0.451 ms；输入拷贝平均 0.788 ms。
- 主要瓶颈在 NPU 图执行，不在后处理或输入拷贝；下一步优先结构化剪枝与通道布局实验。


## 2026-07-10 通道对齐与剪枝准备

- 基线 RKNN 输出通道已呈 64/32/16 等硬件友好对齐形态，当前没有证据表明额外通道填充会降低 NPU 延迟，因此暂不改图做 padding。
- 当前 rknn-env 未安装 torch_pruning 或 nni；结构化剪枝不能直接套现成工具，需先实现可控的低比例通道裁剪并处理 C2f/Concat/Detect 依赖。


## 2026-07-10 剪枝前通道重要性

- 已对六类 YOLOv8s baseline 的 57 个 Conv+BN 层计算 BN gamma 通道重要性，报告位于 /home/rambos/datasets/alertgateway_desktop6_final/runs/baseline_channel_importance.txt。
- 最低均值层为 model.15.cv2/cv1（128 通道）及 model.4.m.*（64 通道）；这些只作为候选，不直接裁剪。
- 下一步需建立依赖感知的结构化裁剪和微调验证。


## 2026-07-10 结构化剪枝计划

- 已生成 docs/artifacts/structured_pruning_plan.json。第一轮候选层：model.4.cv1、model.4.m.0.cv2、model.4.m.1.cv1、model.15.cv1、model.15.cv2，目标比例 10%。
- 约束：通道按 16 对齐，保持 C2f split/concat 和 Detect 输入兼容，剪枝后必须微调再导出。
- 当前仍是计划阶段，未修改基线权重。


## 2026-07-10 torch-pruning 兼容性检查

- 已在 rknn-env 安装 torch-pruning 1.6.1。
- 对 Ultralytics YOLOv8s C2f 内部卷积建立 DependencyGraph 时，model.15.cv1.conv 未被识别进依赖图；直接裁剪存在结构风险。
- 暂不修改权重，需增加 Ultralytics forward wrapper 或自定义 C2f/Concat 依赖映射。


## 2026-07-10 显式层依赖表

- 已生成 docs/artifacts/yolov8s_layer_dependency.tsv，包含 23 个顶层模块的 index/from/type/params。
- 该表用于手写 C2f/Concat/Detect 依赖映射；仍未修改模型权重。


## 2026-07-10 基线与 0.375 结构核对

- 六类 YOLOv8s 基线主干通道为 32/64/128/256/512；0.375 候选为 24/48/96/192/384，确实是不同宽度结构。
- 因此 0.375 的低精度不能归因于训练流程相同，后续剪枝应从基线 32/64/128/256/512 结构出发。


## 2026-07-10 ONNX 融合检查

- 基线原生 ONNX 共 226 个节点，BatchNormalization=0，说明 Conv+BN 已在导出前融合。
- 图中仍有 59 个 Sigmoid、56 个 Mul、13 个 Concat；SiLU 以 Sigmoid+Mul 形式存在，是否由 RKNN 融合需看板端 profiling。
- 不再手工重复做 BN 融合；后续只关注 NPU 对激活和 Concat 的实际耗时。


## 2026-07-10 激活模块替换准备

- 已生成 docs/artifacts/activation_replacement_plan.json，候选为 SiLU（Sigmoid+Mul）替换为 ReLU6。
- 当前仅为实验方案；必须先确认 Sigmoid/Mul 是板端热点，再训练、精度评估、RKNN 测速，基线权重保持不变。


## 2026-07-10 蒸馏方案准备

- 已生成 docs/artifacts/distillation_plan.json。Teacher 为六类 YOLOv8s baseline，Student 为首个验证通过的结构化剪枝模型。
- 蒸馏包含 box、分类 logits、P3/P4/P5 特征，temperature=2.0；验收要求 test mAP50 不低于基线 3 个百分点以内，且 RKNN rknn_run 低于 50.707 ms。


## 2026-07-10 C2f 内部依赖明细

- 已生成 docs/artifacts/yolov8s_c2f_internal.tsv，包含 57 个 C2f/相关内部层的类型、输入输出通道和参数量。
- 该表用于手写剪枝映射，重点约束 cv1/cv2 分支、Bottleneck 序列和 cv3 拼接通道。


## 2026-07-10 任务状态总记录

### 已完成并验证

- 六类 final 数据集已固定，YOLOv8s baseline test mAP50=77.2%。
- 六类 9 输出 ONNX/RKNN 已完成，RKNN 仿真 letterbox test mAP50=56.08%，未发现明显 INT8 量化损失。
- RK3588 基线 profiling：rknn_run 平均 50.707 ms、P90 53.032 ms；outputs_get 4.276 ms；后处理 0.451 ms；输入拷贝 0.788 ms。
- 0.375 宽度模型已完成 30 epoch 微调，但 test mAP50=42.3%，已淘汰。
- ONNX BatchNormalization=0，Conv+BN 已融合；额外通道 padding 暂无收益证据。
- 已生成通道重要性、顶层依赖、C2f 内部结构、剪枝方案、激活替换方案和蒸馏方案等项目附件。

### 当前阻塞

- torch-pruning 1.6.1 无法正确追踪 Ultralytics YOLOv8 的 C2f/Concat/Detect 主干依赖，只识别到末端节点。
- 因此尚未修改 baseline 权重，避免生成结构错误模型。
- 板端完整摄像头 Smoke 仍受 ISP sensor 输入链路阻塞。

### 期望结果

- 手工重建一个约 10% 结构化剪枝的 YOLOv8s baseline；保持通道 16 对齐和六类输出契约。
- 剪枝后 test mAP50 不低于 baseline 3 个百分点以内。
- RK3588 NPU rknn_run 低于 baseline 50.707 ms；否则淘汰该方案。
- 达标后再做必要模块替换和蒸馏，并重新导出 ONNX/RKNN、仿真和板端复测。


## 2026-07-11 手工 10% 通道缩减结果

- 新结构文件：docs/artifacts/yolov8s_pruned10_model.yaml；参数量约 9.51M，相比 baseline 11.14M 减少约 14.6%。
- 1 epoch 烟雾训练通过，权重迁移 124/355，forward/loss 正常。
- 30 epoch val mAP50=58.6%，但固定 test mAP50=64.0%、mAP50-95=42.8%，低于 baseline test mAP50=77.2%。
- 结论：该 10% 结构候选淘汰，不导出 ONNX/RKNN；保留产物用于后续分析。


## 2026-07-11 5% 候选配置检查

- 初版 yolov8s_pruned5_model.yaml 虽主干通道写为 32/64/120/240/480，但实际构建参数量约 7.92M，较 baseline 11.14M 缩减约 29%，不符合 5% 目标。
- 未启动训练；该配置标记为需修正，避免重复过度缩减。


## 2026-07-11 低比例剪枝结果

- 修正后的低比例候选参数约 10.734M，较 baseline 11.138M 减少约 3.6%；1 epoch smoke 和 30 epoch 微调均通过。
- val mAP50=59.3%，固定 test mAP50=58.5%、mAP50-95=38.2%，仍显著低于 baseline test mAP50=77.2%。
- 结论：该低比例候选也淘汰，不导出 RKNN；直接通道缩减路线暂不继续扩大。


## 2026-07-11 ReLU6 模块替换结果

- ReLU6 baseline 结构 smoke 通过，权重迁移 355/355；30 epoch 微调完成。
- val mAP50=63.2%，固定 test mAP50=65.6%、mAP50-95=50.2%，低于 baseline test mAP50=77.2%。
- 结论：ReLU6 替换候选淘汰，不导出 RKNN；当前结构改动均未达标。


## 2026-07-11 蒸馏计划待执行

- 当前尚未创建或运行 distill_desktop6.py。
- 计划：实现 Teacher/Student 联合训练，加入分类 logits、框回归和 P3/P4/P5 特征蒸馏；先 1 epoch smoke，再 30 epoch 完整训练。
- 验收：test mAP50 >= 74.2%，然后才导出 ONNX/RKNN 并测试 RK3588 延迟。


## 2026-07-11 distillation smoke status

- Added tools/dataset/distill_desktop6.py. It reuses the Ultralytics dataloader and detection loss and adds Teacher/Student detection-head logits distillation (64 box channels, class channels, and P3/P4/P5 outputs, temperature=2.0).
- A six-class Teacher and the 10% pruned Student completed all 47 batches of a 1-epoch smoke run. Losses stayed finite (GPU memory about 3.75 GB), proving the distillation forward/backward path runs.
- No reusable best.pt has been produced yet: Ultralytics 8.4.67 reports a missing distilled_forward during checkpoint serialization, and its training validator requests an incompatible auxiliary loss from the inference tensor. Do not start the 30-epoch run yet; next step is a top-level serializable DetectionModel/Trainer subclass plus the fixed evaluator.


## 2026-07-11 distillation result

- Replaced the dynamic forward monkey-patch with top-level `tools/dataset/distill_model.py:DistillDetectionModel`; the Teacher is attached through a non-serialized attribute.
- The 1-epoch smoke completed 47/47 batches and the checkpoint was independently reloadable. A 20-epoch run was interrupted by the session limit after writing `last.pt`; a separate 10-epoch resume completed and produced `best.pt`.
- Independent Ultralytics test evaluation of the final resume checkpoint: mAP50=56.82%, mAP50-95=39.08% on 29 test images / 130 boxes. This is below the distillation acceptance target mAP50 >= 74.2% and below the non-distilled pruned candidate's 64.0%, so the distillation candidate is rejected and must not be exported to ONNX/RKNN.
- The standalone model module remains as a reusable experiment scaffold; next optimization work should return to the baseline or a new student design rather than deploy this checkpoint.


## 2026-07-11 distillation per-class comparison

- Same fixed test evaluation: baseline mAP50=77.18%, distilled pruned candidate=56.82%.
- Per-class AP50 baseline -> distilled: cell phone 79.62% -> 57.99%, cup 89.15% -> 72.21%, keyboard 93.97% -> 83.34%, mouse 52.17% -> 27.17%, laptop 51.99% -> 6.24%, book 96.21% -> 93.97%.
- The regression is concentrated in laptop and mouse, so no further distillation or RKNN export is justified. Keep the baseline as the accuracy reference and treat the distillation code/checkpoint as rejected experiment artifacts.


## 2026-07-11 dataset stats default correction

- `tools/dataset/check_dataset_stats.py` previously defaulted to the superseded `/home/rambos/datasets/alertgateway_desktop6`; its default now points to `/home/rambos/datasets/alertgateway_desktop6_final`.
- Reverified current final data: train/val/test images=372/30/29, boxes=746/87/130, with 48 empty negative train labels. This matches `data.yaml` and the fixed evaluation set.


## 2026-07-11 reusable per-class evaluator

- Added `tools/dataset/evaluate_test_per_class.py`, which evaluates any YOLO checkpoint on the fixed split and prints total mAP50/mAP50-95 plus AP50 for all six classes.
- The evaluator explicitly registers the repository `tools.dataset.distill_model` module before loading custom checkpoints, avoiding the environment's unrelated third-party `tools` package and preventing Ultralytics auto-install fallback.
- Reproduced baseline mAP50=77.18% and rejected distillation mAP50=56.82% with the same tool.


## 2026-07-11 baseline smoke preflight

- Verified local `build/AlertGateway` is an ARM64 ELF executable and the production config uses the baseline contract: /dev/video20, 640x480 at 15 FPS, rockchip_dfl, six target classes, and 2000 kbps stream.
- The configured RKNN model path is `model/yolov8s_rockchip_dfl.rknn`, which is a board-side deployment artifact and is not present in the repository model directory. Local preflight is complete; the remaining Smoke blocker is the board ISP/sensor input chain.


## 2026-07-11 smoke execution order

- User decision: defer board Smoke until the board ISP/sensor input chain is restored.
- Until then, do not change CaptureThread, start new model training, export rejected candidates, or deploy anything. Resume with baseline Smoke first when /dev/video20 and STREAMON are available.

## 2026-07-11 baseline board Smoke verified

- Board ISP and baseline board Smoke verified: video20 YUYV 640x480 at 15 FPS; RKNN loaded; 60-second run ended cleanly; H.264 FLV 57.667 seconds at 640x480/15 FPS.
- MQTT remains unverified because config uses the placeholder broker.

## 2026-07-11 baseline positive-scene verification

- Repeated board run with MQTT ignored completed cleanly. The live scene produced objs:1-2 in the inference log, confirming baseline detection is active on the restored ISP path.
- Current CPU governor was interactive and the sampled cpu0 frequency was 408000, explaining the observed rknn_run variation of about 31-45 ms instead of the earlier controlled ~27 ms benchmark.
- Total inference time in this run was about 46-76 ms; no source or model artifact changes were made.

## 2026-07-11 governor performance comparison

- Temporarily switched CPU frequency policies 0/4/6 to performance for a 10-second board run, then restored all policies to interactive.
- Under performance, rknn_run (npu field) was mostly 27-31 ms, confirming the earlier ~27 ms benchmark. Total inference was mostly 31-37 ms.
- The 30-45 ms interactive result is frequency/scheduling variation, not a model regression.

## Current state correction (2026-07-11)

- The older entries that describe board Smoke as waiting for ISP are historical and superseded. ISP recovery, baseline Smoke, positive-scene detection, and governor performance comparison are complete.
- MQTT is intentionally out of scope for this validation; do not treat the placeholder broker as a remaining blocker.
- Final baseline performance reference: rknn_run about 27-31 ms with performance governor; normal interactive operation varies about 30-45 ms.

## 2026-07-11 graph optimization candidate: official six-class RKNN rebuild

- Rebuilt the existing six-class native 640x640 ONNX through RKNN simulator using the fixed 150-image calibration set and evaluated the fixed 29-image test split.
- After fixing the evaluator input batch dimension, the letterbox result was mAP50=56.08
## 2026-07-11 graph optimization candidate: official six-class RKNN rebuild

- Rebuilt the existing six-class native 640x640 ONNX through RKNN simulator using the fixed 150-image calibration set and evaluated the fixed 29-image test split.
- After fixing the evaluator input batch dimension, the letterbox result was mAP50=56.08, with class AP50: cell phone 56.25, cup 38.00, keyboard 76.92, mouse 46.35, laptop 27.27, book 91.70.
- This candidate is far below the baseline mAP50=77.18 and the acceptance gate about 74.2; it is rejected and must not be exported or deployed.
- The failure is an accuracy result, not an RKNN build failure. Do not spend board Smoke time on this candidate.
- Fixed tools/dataset/evaluate_rockchip_rknn_yolo.py to pass the required batch dimension to RKNN simulator inference.

## 2026-07-11 persistent performance optimization deployed

- Deployed and enabled the existing tools/performance/rockchip-performance.service on firefly@192.168.0.200.
- Verified CPU policies 0/4/6, NPU, and DMC/DDR are performance; CPUIdle state1 is disabled. Frequencies were 1800/2256/2256 MHz, NPU 1000 MHz, DMC 2112 MHz.
- A production-path 15-second Smoke after deployment completed cleanly. The observed npu/rknn_run time was about 26.7 ms and total inference about 33.9 ms.
- This is a persistent board performance optimization, not a model replacement. The baseline RKNN artifact and application source were unchanged.

## 2026-07-11 persistent performance long-run verification

- With rockchip-performance.service active, a 120-second production-path baseline run completed with clean Shutting down... / Done.
- Final samples stayed around rknn_run 26.7 ms and total inference 33.9-34.5 ms.
- Thermal readings after the run were approximately 37.0-37.9 C across the reported zones; no thermal throttling or process failure was observed.
- Persistent performance policy is accepted as the current board optimization baseline, with the usual power-consumption tradeoff.

## 2026-07-11 five-minute performance-service run

- A 300-second production-path run under rockchip-performance.service completed on schedule and exited cleanly with Done.
- The run continued to produce detections (objs:1 observed near the end), with rknn_run about 26.7 ms and total inference about 30-35 ms.
- CPU/NPU frequencies remained at the locked performance values during the sampled interval. Thermal readings peaked around 37.9 C and returned to about 35-36 C after shutdown; no throttling or process failure was observed.
- The persistent performance policy is accepted for the current board baseline. No board power sensor was available in this run, so absolute power consumption remains unmeasured.

## 2026-07-11 power measurement availability

- Checked the board for power_supply and hwmon sensor nodes; none are exposed through sysfs.
- In-board power comparison cannot be completed with software-only measurements. It requires an external USB-C power meter or a board-level meter.
- Performance optimization remains accepted based on latency, stability, and thermal evidence; power measurement is an external follow-up, not a software blocker.

## 2026-07-11 production RKNN hotspot baseline

- Ran the board benchmark against the deployed baseline RKNN with zero-copy input, SRAM enabled, all NPU cores, 50 warmups, and 300 measured runs while rockchip-performance.service was active.
- Median timings: rknn_run 26.540 ms (P90 26.617), outputs_get 1.723 ms (P90 1.750), Rockchip postprocess 4.126 ms (P90 4.194), complete run_get_post 32.395 ms (P90 32.534).
- The NPU remains the dominant cost, but CPU output handling/postprocess is about 5.8 ms median combined. Any next software optimization should target output conversion/postprocess only if it preserves detection results; do not change the model blindly.

## 2026-07-11 RKNN per-operator graph profiling

- Added an opt-in perf-detail mode to tools/benchmark/rknn_benchmark.cpp. It initializes RKNN with collect-perf and prints one post-outputs-get report, so normal multi-run latency benchmarks remain unaffected.
- Board run used the deployed 9-output YOLOv8s RKNN, RKNN API 2.3.0, all NPU cores, SRAM, zero-copy, warmup=1/runs=1. Operator total was 25.874 ms: ConvExSwish 22.222 ms (85.89%), Concat 1.275 ms (4.93%), Split 0.810 ms (3.13%), ConvSigmoid 0.507 ms (1.96%), Resize 0.352 ms (1.36%).
- RKNN already fuses SiLU into ConvExSwish. All material convolution widths are 32/64/128/256/512 aligned. Therefore channel padding has no justified target, and ReLU-style activation substitution remains rejected by accuracy.
- Concat is measurable but not a standalone hotspot. Do not manually replace/reorder it before an individually equivalent A/B shows a net benefit; splitting its following convolution into branches would add convolution launches and is unlikely to beat the current 4.93% cost.
- Next graph-level action: retain this profiler and only investigate a structure-changing candidate if it can reduce the dominant ConvExSwish work while preserving the fixed six-class accuracy gate. Do not alter the deployed model based on this profiling run.
## 2026-07-11 depth-reduction graph candidate in progress

- Per-operator profiling showed ConvExSwish as the dominant cost. Previous 3.6% and 14.6% direct channel-reduction candidates failed fixed-test accuracy, so no further channel-width reduction is being tried.
- New isolated candidate removes one Bottleneck from only the 80x80 backbone C2f stage. It preserves all channel widths, Detect inputs, and Concat contracts. The candidate has 11.056M parameters and 27.4 GFLOPs versus the 28.6 GFLOPs baseline.
- One-epoch smoke training completed and produced a reloadable checkpoint; fixed test mAP50=26.93%, which is not a decision metric after one epoch. The independent 30-epoch fine-tune is running as yolov8s_depth4_reduced_30e_20260711. Do not export, deploy, or judge this candidate until the fixed test evaluation completes.
## 2026-07-11 depth-reduction graph candidate result

- The isolated 80x80 C2f depth-reduction candidate completed 30 epochs. It retained 11.046M parameters and reduced compute from 28.6 to 27.4 GFLOPs.
- Fixed independent test: mAP50=66.44%, mAP50-95=54.57%, below the acceptance gate mAP50 >= 74.18% and the baseline 77.18%. AP50 baseline comparison shows notable regressions in keyboard and mouse; this is not a deployable trade-off.
- Decision: reject candidate. Do not export ONNX/RKNN or deploy it. Direct C2f-depth reduction now joins channel-width reduction and ReLU6 replacement as rejected graph-level routes for this dataset.
## 2026-07-11 DFL INT8 decode micro-optimization rejected

- Tested direct INT8 DFL softmax to avoid per-candidate float staging. The first A/B appeared faster, but rknn_run and outputs_get changed simultaneously, so it was not attributable to the code change.
- In controlled interleaved hot-state 300-run measurements, baseline and optimized postprocess medians remained about 4.15 ms and 4.00 ms respectively, within run-to-run variation; complete median remained about 32.4-32.5 ms.
- The code change was reverted and is not part of the deployment candidate. Next output-path experiment is RKNN output-memory prebinding in the benchmark only, with explicit cache synchronization and detection-equivalence checks before any production integration.
## 2026-07-11 output preallocation result

- Benchmark-only caller-owned raw output buffers reduced synthetic deterministic-input outputs_get median from about 1.73 ms to 1.17 ms and complete run_get_post from about 32.53 ms to 31.77 ms in interleaved 300-run tests. Detection result counts matched on the same input.
- The same mechanism was tested in an isolated production binary with the existing model/config. It initialized and exited cleanly, but real board tail samples were baseline 29.26-29.37 ms versus candidate 29.30-29.48 ms; no stable production gain was demonstrated.
- Production output preallocation was reverted. The benchmark option remains for future scene-specific testing; do not deploy it based on the synthetic benchmark alone.
## 2026-07-11 positive-scene output preallocation validation

- Repeated the output-buffer A/B on a fixed real desktop test image containing cup/book/phone/keyboard content, letterboxed to 640x640 and uploaded to the board.
- Default and preallocated paths produced exactly the same final six detections, including class IDs, scores, and box coordinates.
- Timing remained state-sensitive: the preallocated path showed outputs_get median about 1.17 ms in one run, but real production Smoke and positive-scene runs did not show a stable complete-chain gain. Keep the production path unchanged.
- Current software optimization conclusion: no validated postprocess/output change is ready for deployment. Further work needs a stable target scene captured directly from the board or a new profiling method that separates RKNN scheduling from CPU postprocess.
## 2026-07-11 board CPU profiling availability

- The board has /usr/bin/perf and /usr/bin/gprof names, but perf cannot record because linux-tools for the running 6.1.118 kernel are unavailable. No source change was made for this failed profiling attempt.
- If further CPU-stage optimization is required, add optional benchmark-internal phase timers for candidate scan/DFL and NMS instead of relying on kernel perf.
## 2026-07-11 direct perf tool resolved

- linux-tools packages were already installed on the board: linux-tools-generic and linux-tools-5.15.0-185. The /usr/bin/perf wrapper rejected the vendor 6.1.118 kernel, but direct /usr/lib/linux-tools-5.15.0-185/perf works for stat and record.
- Fixed positive-scene perf sample: RockchipYoloPostprocess::process accounted for 66.87% of sampled cycles, rknn_outputs_get 19.41%, rknn_run 8.77%, nms only 0.23%, and expf 1.38%.
- This rules out NMS as the next target. Continue by splitting process into candidate scan/class filtering versus DFL decode timing; do not install mismatched kernel packages or change production code from the perf sample alone.

## 2026-07-11 postprocess loop optimization and cache validation

- Profiling and Hotspot Analysis: The previously reported perf recording issue was due to the `/usr/bin/perf` wrapper version mismatch on the running 6.1.118 kernel. Directly running `/usr/lib/linux-tools-5.15.0-185/perf` successfully bypasses this mismatch. Compiled `rknn_benchmark` with debug symbols (`-g -O2`) and ran `perf record`/`perf annotate`. The bottleneck inside `RockchipYoloPostprocess::process` was identified in class filtering: `int8_t score = score_tensor[class_id * grid_len + cell]`. The original layout had `cell` in the outer loop and `class_id` in the inner loop, causing non-contiguous memory loads (stride of `grid_len` up to 6400 bytes) and cache thrashing. Additionally, `sum_tensor` filtered out only 4.6% of cells (8016/8400 passed), making the cache thrashing critical.
- Implementation: Restructured the loop to place `class_id` in the outer loop and `cell` in the inner loop, changing memory access to 100% sequential. Added stack-allocated `best_scores` and `best_classes` buffers (falling back to heap vectors only for `grid_len > 6400`) to avoid allocation overhead.
- Correctness and Verification: Confirmed that detection outputs (class IDs, scores, and coordinates) are 100% identical on the fixed positive-scene test image.
- Performance Gains: 300 runs of the board benchmark showed `postprocess` median latency reduced from **4.139 ms** to **2.842 ms**, yielding a net benefit of **1.297 ms** (31.3% post-processing speedup). The total pipeline (`run_get_post`) median latency fell from **32.833 ms** to **31.597 ms**.
- Deployment: Because the gain exceeds the 0.3 ms threshold and achieves 100% output equivalence, the optimized post-processing was integrated into `src/infer/RockchipYoloPostprocess.cpp` for production use. Following successful candidate verification, the optimized binary was officially deployed on the board (replacing `~/AlertGateway/AlertGateway`, with a backup saved to `~/AlertGateway/AlertGateway.bak.20260711_205840`). The newly deployed official program was verified via a 60-second smoke test, confirming normal model loading, inference, encoding, and graceful exit behavior.

## 2026-07-11 post-deployment profiling and stage timing closure

- Latency Closure & Stage Timing: Re-ran the stage-level timed benchmark on the board for 300 runs under the persistent performance service. Latency medians are: `rknn_run_wall` = 26.58 ms, `outputs_get` = 1.21 ms, `postprocess` = 2.86 ms, and `run_get_post` = 30.65 ms.
- Stage-Level Breakdowns:
  - `stage_scan` (candidate scan/class filter): **2.642 ms** (92.4% of post-process time)
  - `stage_dfl` (DFL decode): **0.135 ms**
  - `stage_box_map` (coordinate mapping): **0.009 ms**
  - `stage_nms` (NMS box deduplication): **0.017 ms**
- Equivalence: Confirmed that detection outputs (class IDs, confidence scores, bounding box coordinates) from the timed post-processing path match the baseline path 100% exactly.
- Final Optimization Closure: The total time for DFL, box mapping, and NMS combined is only **0.161 ms** (much lower than the 0.3 ms threshold). This proves there is no independent optimization space remaining outside the class-filtering loop. As the class-filtering loop is already optimized for cache locality and further NEON vectorization is high-risk and has low return, the CPU post-processing optimization is officially closed. Any future latency improvements should focus on physical power measurements or a new network model architecture/larger datasets.