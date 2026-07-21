# AlertGateway 项目记忆

最后更新：2026-07-21

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

- Git分支：`master`；当前已推送 HEAD 为 `2938280`（2026-07-19），本地与 `origin/master` 一致；当前工作区另有未提交的 README 与记忆文件更新。
- `b00d067` 已纳入检测框运动诊断、分析工具、实验配置/报告和对应记忆更新；生产视觉逻辑仍以
  `f3970aa` 为基线，诊断开关默认关闭。后续工作区变更不得直接清理或用 Git 回退覆盖。
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

## 2026-07-11 新开发方向：单路 4K 拉流 + 图像处理

架构设计已确认，文档位于 `docs/4k/4k_pull_stream_image_processing_plan.md`，后续开发以其为准。

### 核心决策

- **视频来源**：V4L2 本地采集 与 RTMP/RTSP 拉流（SRS 中转）均为一等可配置项，通过 `source.type` 切换，互不废弃。
- **ROI 坐标系**：归一化浮点 0.0～1.0，与分辨率解耦。
- **Tiling 范围**：仅对命中 ROI 的区域做 Tiling，单帧推理次数控制在 1～2 次；无 ROI 命中时回退整图缩放推理。
- **现有代码边界**：`CaptureThread` / `InferThread` / `EncodeThread` 核心逻辑保持不变。

### 模块开发状态

| 模块 | 说明 | 状态 |
|---|---|---|
| `src/capture/IVideoSource.hpp` | 视频源抽象接口 | **已完成** |
| `src/capture/PullStreamThread` | FFmpeg RTMP/RTSP 拉流线程 | **已完成** |
| `src/infer/ThumbnailTask` | 缩略图生成，附加到 MQTT payload | 待开发 (P1) |
| `src/infer/RoiFilter` | ROI 过滤 + 帧间 IoU 追踪 + 停留事件 | 待开发 (P1) |
| `src/infer/TilingTask` | ROI 内 Tiling 推理 + 全局 NMS 合并 | 待开发 (P2) |

### 现有文件适配情况 (阶段一已完成)

- `CaptureThread.hpp`：已继承 `IVideoSource` 接口。
- `Frame.hpp`：已新增 `pixel_format` 字段及 `PixelFormat` 格式枚举。
- `main.cpp`：已支持 `source` 节点及 `source.type` 路由动态创建视频源，向下兼容 `camera`。
- `InferThread.cpp`：已支持根据 `pixel_format` 路由 RGA 输入格式，并编写 `nv12_to_rgb` CPU 兜底转换。
- `EncodeThread.cpp`：已支持对 `NV12` 输入帧绕过 YUYV→NV12 转换直通 MPP 编码缓冲区。
- `CMakeLists.txt`：已注册 `PullStreamThread.cpp`。

### 当前验证状态

- **交叉编译**：主机端编译 100% 通过（AlertGateway / rknn_benchmark / infer_camera_smoke 三目标无错）。
- **板端回归验证**：已部署至开发板，以 V4L2 测试配置运行 15 秒回归测试。NPU 耗时约 **26.6 ms**，全链路耗时约 **32.5 ms**，表现与优化后基线一致，无任何逻辑退化。退出接收 SIGTERM 正常优雅注销。
- **下一步状态**：P0（拉流与抽象）阶段完全关闭，下一步进入 P1（三类图像处理任务）开发。

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

## 2026-07-11 新开发方向：单路 4K 拉流 + 图像处理

架构设计已确认，文档位于对话 artifact `implementation_plan.md`，后续开发以此为准。

### 核心决策

- **视频来源**：V4L2 本地采集 与 RTMP/RTSP 拉流（SRS 中转）均为一等可配置项，通过 `source.type` 切换，互不废弃。
- **ROI 坐标系**：归一化浮点 0.0～1.0，与分辨率解耦。
- **Tiling 范围**：仅对命中 ROI 的区域做 Tiling，单帧推理次数控制在 1～2 次；无 ROI 命中时回退整图缩放推理。
- **现有代码边界**：`CaptureThread` / `InferThread` / `EncodeThread` 核心逻辑保持不变。

### 新增模块（待开发）

| 文件 | 说明 |
|---|---|
| `src/capture/IVideoSource.hpp` | 视频源抽象接口 |
| `src/capture/PullStreamThread` | FFmpeg RTMP/RTSP 拉流线程 |
| `src/infer/ThumbnailTask` | 缩略图生成，附加到 MQTT payload |
| `src/infer/RoiFilter` | ROI 过滤 + 帧间 IoU 追踪 + 停留事件 |
| `src/infer/TilingTask` | ROI 内 Tiling 推理 + 全局 NMS 合并 |

### 需最小改动的现有文件（待开发时改动）

`CaptureThread.hpp`（加继承）、`Frame.hpp`（加 pixel_format 字段）、`main.cpp`（source 工厂）、`InferThread.cpp`（格式判断 + 任务入口）、`EncodeThread.cpp`（NV12 直通路径）、`config.json`（新增 source / image_processing 节点）、`CMakeLists.txt`（注册新源文件）。

### 验收门槛

- 阶段一：`source.type=v4l2` 行为与现有完全一致；`pull_stream` 稳定拉流 60 秒无错误。
- 阶段二：三类任务全部 disabled 时零影响；各任务独立验收。

### 当前状态

- 架构设计已确认，**源码零修改**，等待开发启动指令。
## 2026-07-12 4K pull-stream stage-one remediation

- The previously recorded source-code-unchanged state is obsolete. Stage-one source-selection code now exists in the working tree: IVideoSource, PullStreamThread, Frame pixel format, NV12 direct encoding, source factory wiring, and source configuration.
- The V4L2 path was restored to its prior single-planar YUYV implementation; its only stage-one architectural change is IVideoSource inheritance. This preserves the existing camera behavior until an independently tested mplane change is requested.
- PullStreamThread now allocates its AVFormatContext before opening the source and installs an FFmpeg interrupt callback backed by running_, so shutdown can interrupt blocked network I/O before joining the worker.
- Pull-stream frames are accepted only when their decoded width/height exactly equal configured source.width/source.height and are even. Mismatches are dropped before either inference or the fixed-size encoder path, preventing invalid NV12 copies.
- config/config.json now uses the source schema with the existing V4L2 defaults. The image_processing task switches are explicitly disabled placeholders; Thumbnail, ROI, and Tiling remain unimplemented and must not be enabled or considered accepted.
- Host cross-build completed successfully after these changes. Board acceptance remains pending: V4L2 regression, a 60-second RTMP/RTSP run, disconnect/reconnect, blocked-read shutdown, and a positive configuration-mismatch check.

## 2026-07-12 image-processing implementation

- Added ImageProcessingConfig parsing for thumbnail, ROI, and tiling settings. The checked-in default configuration keeps all three tasks disabled.
- Added RoiFilter: normalized ROI coordinates are converted to frame pixels; detections outside configured regions can be filtered; optional IoU-based dwell tracking emits roi_events.
- Added TilingTask: ROI tile geometry, packed YUYV/NV12 crop extraction, coordinate offset, and class-aware global NMS merge. The current scheduler keeps the existing full-frame inference and allows at most one additional ROI tile refinement, so the per-frame inference cap remains two.
- Added ThumbnailTask: nearest-neighbor NV12 thumbnail generation for YUYV and NV12 frames. MQTT payload includes width, height, format, and Base64 NV12 data. JPEG/RGA thumbnail encoding remains a follow-up optimization; the current CPU path is correctness-first.
- InferThread now applies ROI filtering, optional tile refinement, thumbnail generation, and extended MQTT payload fields after postprocess. Image-processing modules are registered in both production and camera-smoke targets.
- Added tools/benchmark/image_processing_smoke.cpp and the BUILD_IMAGE_PROCESSING_SMOKE CMake option. Cross-build of AlertGateway, infer_camera_smoke, rknn_benchmark, and image_processing_smoke passes.
- Board validation is still required before enabling any task in production: ROI coordinate tests, tile recall/duplicate suppression, thumbnail visual inspection, MQTT payload size, and latency/RSS measurements.

## 2026-07-12 testsrc2 4K synthetic stream

- Generated runs/testsrc2/testsrc2_4k_60s.mp4: H.264 Constrained Baseline, 3840x2160, 15 FPS, 900 frames, 60 seconds, approximately 280 MB. FFprobe metadata and full host decode completed successfully.
- Added tools/test/push_testsrc2_4k_to_srs.sh. It validates the input with ffprobe and pushes the file in real time with -re -stream_loop -1 to the local SRS test stream.
- Verified a real WSL-to-SRS push to rtmp://192.168.0.168/live/testsrc2: approximately 898 frames were sent over 60 seconds at about 15 FPS. The non-zero script exit is from the intentional SIGINT timeout; the RTMP session and frame transfer completed normally.
- Added runs/testsrc2/config_testsrc2_4k.json for the board to pull the test stream from SRS. The board-side process still needs to be started separately; no RKNN inference result has been observed yet.

## 2026-07-12 board deployment check

- Attempted the planned SSH deployment check to .
- The endpoint accepted the key but reported hostname  and WSL2 kernel  on , not the RK3588 board. No files were copied and no process was started.
- Do not deploy to this endpoint until the actual board address is identified; the 4K SRS input remains available at .
Board 4K validation 2026-07-12: Windows SSH key access restored. ARM64 binary and config_testsrc2_4k.json deployed to firefly 192.168.0.200. 35 second run connected to rtmp://192.168.0.168/live/testsrc2, opened 3840x2160 yuv420p, initialized RKNN rockchip_dfl and MPP H264 encoding, connected MQTT, and ran inference continuously. NPU about 25.9-27.3 ms, total about 36-45 ms, encoder 15.1 fps, clean timeout shutdown. Initial avformat errors were due to inactive SRS publisher.
VERIFIED 2026-07-12: board 192.168.0.200 deployed ARM64 AlertGateway and config_testsrc2_4k.json; pulled 3840x2160 yuv420p from SRS rtmp://192.168.0.168/live/testsrc2 for 35 seconds; RKNN rockchip_dfl, MPP H264, MQTT, inference and clean timeout shutdown all succeeded; NPU 25.9-27.3 ms, total 36-45 ms, encoder 15.1 fps.
SRS verification 2026-07-12: HTTP API 192.168.0.168:1985 reports SRS 5.0.205 (major 5 minor 0 revision 205), pid 1622; RTMP port 1935 is reachable.
2026-07-12 30 FPS 4K validation: PullStreamThread now accepts AV_PIX_FMT_YUVJ420P using the existing YUV420P to NV12 conversion path. Rebuilt and deployed ARM64 binary. Original 3840x2160 30 FPS phone stream connected successfully; detections were observed with objs 2-6. Inference total was about 41-48 ms and NPU about 27-30 ms, implying roughly 21-24 processed FPS under this 4K workload. MQTT and clean timeout shutdown succeeded.
2026-07-12 host self-test: rebuilt AlertGateway, infer_camera_smoke, image_processing_smoke, and rknn_benchmark successfully; push script bash syntax and targeted diff check passed; 4K source metadata verified as 3840x2160 H264 yuvj420p 30 FPS; synthetic source verified as 3840x2160 H264 yuv420p 15 FPS. ARM smoke binaries are cross-compiled and require board execution.
2026-07-12 stage-one stability: Parallel 85-second 30 FPS 4K publisher and 65-second board run completed with successful yuvj420p decode, continuous objects detections, MQTT connection, and clean timeout shutdown. Stage-one 60-second stability evidence is complete. A publisher stop/restart reconnect attempt was inconclusive because the WSL publisher command produced no retained board log; do not mark reconnect acceptance until rerun with captured logs.
2026-07-12 reconnect test: Scripted two-phase publish was executed. Board log confirmed Connection closed. Reconnecting after publisher stop, but no second Successfully connected line appeared before the 60-second board timeout. Mark disconnect detection as passed and automatic recovery as pending rerun with a longer second publish window and captured logs.
2026-07-12 strict reconnect acceptance passed: SRS was restarted and verified empty before test. First publisher was confirmed active by SRS API; board connected at log line 14. Publisher PID 18243 was force-killed; board produced av_read_frame Input/output error and entered reconnect retries. A second publisher was started; board connected again at log line 995 and continued inference with objs detections. Added rw_timeout=5000000 to PullStreamThread; removed the invalid generic timeout option that caused RTMP listen_timeout errors. This completes real disconnect and recovery evidence for stage one.
STAGE-1 ACCEPTED 2026-07-12: Strict RTMP disconnect/recovery test passed after SRS restart. SRS API confirmed no stale streams; first publisher was API-active; board connected; publisher PID 18243 was force-killed; board reported av_read_frame Input/output error, entered reconnect retries, then connected again after second publisher started and resumed objs detections. PullStreamThread now uses rw_timeout=5000000 and no invalid generic timeout option. Stage-one pull-stream acceptance is complete for 4K RTMP path; V4L2 camera regression and VLC/MQTTX manual inspection remain separate follow-ups.
2026-07-12 V4L2 regression passed: UVC Camera at /dev/video20 negotiated YUYV 1280x720, actual fps 10/1 despite requested config fps 15. Board ran 65 seconds with RKNN inference, MPP 1280x720 encoding, MQTT connection, and clean timeout shutdown. Detection objs 0-1 appeared repeatedly; encode reported 10 fps. Stage-one V4L2 behavior is accepted with the camera-supported 10 FPS limitation.



## 2026-07-12: MJPEG support deferred

User confirmed not to implement V4L2 MJPEG now. Keep current YUYV behavior unchanged. Future task: add MJPEG negotiation and JPEG decode to NV12, then test 1280x720 at 15 FPS and 30 FPS on board. Keep 640x480 and 720p configurable.

## 2026-07-12: 阶段二图像处理功能验收通过

- **回归测试**：Thumbnail/ROI/Tiling 默认 disabled 状态下的 V4L2 摄像头回归测试成功，编码推流 29 FPS，MQTT 正常发送，同阶段一行为一致，退出正常。
- **Thumbnail 验收**：启用后在 4K 流中生成 NV12 320x180 缩略图；MQTT payload 包含 `thumbnail` 字段，Base64 数据长度为 115200，解码后二进制长度 86400 字节，精确对齐 `320 * 180 * 1.5`；MQTT 消息能被正常解析，连续运行 65 秒正常。
- **ROI 验收**：配置左半区域为 ROI (x=0, y=0, w=0.5, h=1.0)，ROI 外目标被成功过滤，ROI 内目标正常上报并保留全图坐标；`track_dwell_sec=2.0` 能正常生成 `roi_events`（包含 region, label, dwell_sec 字段），在 MQTT payload 中正确输出，且带有 `roi_events` 的消息成功绕过 objects 去重逻辑。
- **Tiling 验收**：配置 2x1 局部 Tiling，在 tiling 激活且 ROI 存在时自动跳过全局推理，仅执行 2 个 Tile 的局部推理，将单帧 NPU 推理次数限制在 2 次；Tile 坐标映射回全图正确，合并后全局 NMS 去重正常；连续运行 65 秒正常。
- **配置异常验收**：配置 1920x1080 但输入仍为 3840x2160 时，解码帧在 `PullStreamThread` 被丢弃并输出 mismatch 日志，不投递给编码与推理线程，程序不崩溃且各线程正常退出。
- **稳定性和性能**：全图像处理启用时在板端稳定运行 65 秒，并成功通过 SIGINT 信号优雅退出。NPU 延迟在 26-29 ms；编码帧率在 25-36 FPS 范围内；实际 RSS 物理内存全程在 225MB - 274MB 之间波动（5s: 246MB, 60s: 262MB），无任何累积与内存泄漏趋势。
- **4K 输出流播放验证**：实时推送 `testsrc2_4k_60s.mp4` 视频流至 SRS，板端成功拉取 3840x2160@15 FPS 视频进行推理、编码并推送回 `testsrc2_result`。SRS API 实时确认 `testsrc2_result` 为 `Active=True`，分辨率 `3840x2160`，格式 `H.264`，frames 持续稳定增长，连续测试 60 秒以上且正常通过 SIGINT 退出。




## 2026-07-12: 4K output playback verification

Manual playback verification completed. rambosplayer successfully displayed the board output stream from SRS at
tmp://192.168.0.168/live/testsrc2_result. The stream corresponds to the 3840x2160 4K test input and confirms the end-to-end path: WSL publisher -> SRS -> board pull/inference/MPP encode -> SRS output -> rambosplayer. This is an additional visual confirmation for stage-one/stage-two acceptance. V4L2 MJPEG and 720p@15/30 remain deferred enhancements.

## 2026-07-13 4K pull/process/push stutter and quality baseline

- Reproduced the reported 4K path on board 192.168.0.200 using VID_20260712_131410.mp4 (3840x2160, about 30.03 FPS, about 42.8 Mbps) through SRS 192.168.0.168.
- With config_testsrc2_4k.json, the board pulled and decoded 3840x2160 yuvj420p, ran RKNN and MPP continuously, and exited cleanly. Inference totals were about 36.5-49.1 ms/frame; NPU about 25.8-29.2 ms.
- An 18-second capture of testsrc2_result measured 3840x2160 H.264, nominal 15 FPS, average about 15.17 FPS, and about 3.11 Mbps. The very low 3 Mbps 4K output bitrate is a confirmed quality-loss factor; frame pacing/jitter remains to be measured separately.
- Detailed record: docs/4k/4k_pull_stream_reproduction_20260713.md; raw board log and metrics are under runs/testsrc2/. No implementation changes were made for this issue.

## 2026-07-13 4K frame pacing fix and 6 Mbps verification

- Instrumentation was added to PullStreamThread, EncodeThread, and StreamThread for 10-second input/output FPS, queue depth, drop, write-failure, processing-time, and bitrate summaries.
- The first 6 Mbps run confirmed the mismatch: source and encoder/RTMP packet rates were about 30 FPS while the configured target was 15 FPS.
- PullStreamThread now uses the input stream frame rate to compute a frame-step limiter; for 30 FPS input and 15 FPS target it keeps one frame out of every two. PTS gating remains the fallback when input rate is unknown or not above target.
- Board verification after the fix: source about 30.07-30.13 FPS, encoded/RTMP output about 14.97-15.02 FPS, actual bitrate about 5.92-5.98 Mbps, queues remained 0, and encode/output/write failure counters stayed at 0.
- Detailed record: docs/4k/4k_framerate_fix_20260713.md; isolated 6 Mbps config: runs/testsrc2/config_testsrc2_4k_6m.json. Next action is 6/8/12 Mbps visual quality comparison while retaining the fixed pacing.

## 2026-07-13 4K bitrate matrix after frame pacing

- Completed fixed-pacing bitrate matrix at 3/6/8/12 Mbps using the same 3840x2160 input and 15 FPS target. All stable windows stayed around 15.03-15.05 FPS with zero encode/output/write failures and zero queue depth.
- Measured output bitrates were approximately 3.16, 5.98, 7.94, and 11.87 Mbps respectively; stable stream statistics tracked the targets closely.
- 8 Mbps was independently rerun after an interrupted matrix publisher and completed successfully. No production bitrate was selected yet because visual quality inspection is still required. Detailed matrix: docs/4k/4k_bitrate_matrix_20260713.md.

## 2026-07-13 4K bitrate visual comparison preparation

- Extracted 1-second frames from the 3/6/8/12 Mbps captures and found a common desk scene across the four runs; comparison artifacts are under runs/testsrc2/quality_compare_20260713/.
- The earlier 5-second screenshots were not same-scene because each publisher started at a different capture offset and must not be used for a strict quality conclusion.
- Initial inspection indicates 3 Mbps has the most visible compression, 6 Mbps is materially better, and the incremental gain from 8 to 12 Mbps is smaller. This remains a preliminary observation because motion and capture offset still need to be eliminated.
- Keep the production configuration at 3 Mbps. Next action is one strictly synchronized 6-vs-8 Mbps run before selecting a production bitrate.

## 2026-07-13 synchronized 6/8 Mbps verification

- Added tools/test/run_4k_sync_compare_6v8_20260713.sh and ran synchronized 4K 6 Mbps and 8 Mbps cases with the same source, 15 FPS target, and board binary.
- 6 Mbps valid capture: 15.93 seconds, 3840x2160 H.264, about 5.83 Mbps overall capture rate; stable EncodeStats/StreamStats around 15.03 FPS and 5.96 Mbps, queues/failures zero.
- The first synchronized 8 Mbps attempt was invalid because the source RTMP publisher broke immediately after the previous session. It was rerun independently.
- 8 Mbps retry valid capture: 15.53 seconds, 3840x2160 H.264, about 8.04 Mbps overall capture rate; stable EncodeStats/StreamStats around 14.89 FPS and 8.00 Mbps, queues/failures zero.
- Same-time screenshots were extracted under runs/testsrc2/quality_compare_20260713/. The two runs are stable, but source motion still prevents a pixel-perfect visual score; keep production at 3 Mbps until final human review selects 6 or 8 Mbps.

## 2026-07-13 4K 6 Mbps acceptance and production-config correction

- The checked-in config/config.json and board production config are V4L2 camera configurations, not the 4K pull-stream path. An attempted 6 Mbps production verification used the wrong source and was discarded.
- Restored the original V4L2 configurations (local config bitrate 2000 kbps; board config backup restored at 3000 kbps). The board's original config is preserved as config/config.pre_6m_20260713.json.
- Added docs/4k/4k_6m_acceptance_20260713.md and ran the dedicated 4K pull-stream 6 Mbps acceptance with config_testsrc2_4k_6m.json.
- Valid 51.933-second capture: 3840x2160 H.264, about 6.03 Mbps, nominal 15 FPS and average 15.17 FPS. Stable windows were 14.86-15.17 FPS and 5.91-6.08 Mbps; queues returned to zero and put_fail/out_drop/write_fail remained zero.
- Do not change the V4L2 production bitrate based on this test. Treat 6 Mbps as the validated 4K pull-stream candidate; any production switch requires a separate decision about changing the production source path.

## 2026-07-14 4K detection-label scaling

- Detection label rendering in EncodeThread was fixed at 8x16 ASCII / 16x16 Chinese glyphs, which is too small on a 3840x2160 display.
- Added resolution-aware integer scaling: 4K frames use 2x glyphs (about 32 px high), while 1080p and below keep the original size. Background strip padding and text width now scale with the glyphs.
- Cross-build passed. Deployed the updated test binary as ~/AlertGateway/AlertGateway.labels_20260714 and restarted the live 4K 6 Mbps test stream. A verification frame is saved at runs/testsrc2/quality_compare_20260713/frame_labels_4k_2x_20260714.png.
- Existing V4L2 production configuration was not changed.

## 2026-07-14 4K 12 Mbps vs 18 Mbps quality comparison

- Completed the strictly synchronized 4K 12 Mbps vs 18 Mbps video quality comparison under `runs/testsrc2/quality_compare_12v18_20260714/`.
- Both runs successfully completed at 15 FPS: 12 Mbps output average rate was 11.94 Mbps, 18 Mbps was 17.98 Mbps. Board stats showed zero queue build-ups and zero encode/drop/write failures.
- Frame alignment was performed dynamically using downsampled MSE search, mapping frame 100 in outputs to frame 195 in the 30 FPS source video.
- Calculated grayscale PSNR/SSIM for 12 Mbps were 28.2893 dB / 0.5514, and for 18 Mbps were 28.2878 dB / 0.5505.
- Detailed report and side-by-side comparison images are stored in [README.md](file:///home/rambos/arm_test/AlertGateway/runs/testsrc2/quality_compare_12v18_20260714/README.md).

## 2026-07-14 corrected 30 FPS 12/18 Mbps quality comparison

The first automated 12/18 Mbps report was invalid because it used the old AlertGateway binary and board configs at 15 FPS. The corrected run uses AlertGateway.maxfps_20260714 with 30 FPS configs. 12 Mbps measured 12007.9 kbps, 29.99 FPS, PSNR 28.4200 dB, SSIM 0.5735; 18 Mbps measured 18087.0 kbps, 30.08 FPS, PSNR 28.4099 dB, SSIM 0.5813. Both had zero write failures and zero queue depth. 18 Mbps has a small SSIM advantage in this single aligned frame; PSNR is effectively tied, so 12 Mbps remains a valid bandwidth-efficient setting. Artifacts: runs/testsrc2/quality_compare_12v18_20260714/.

## 2026-07-14 V4L2 4K output probe

- 板端 `/dev/video20` UVC 摄像头能力表最高为 1280x960 YUYV/MJPG，不支持 3840x2160。
- 临时 V4L2 配置请求 3840x2160 时，驱动实际协商为 1280x960，`CaptureThread` 按设计报
  `V4L2 resolution mismatch` 并拒绝启动；生产配置未修改。
- 临时 V4L2 配置请求 1280x720 时，程序正常打开并按 1280x720 初始化 MPP/推流，驱动实际
  帧率为 10 FPS（请求 15 FPS 未被满足）。
- 当前 `main.cpp` 将源尺寸同时传给 EncodeThread/StreamThread，编码路径没有独立输出尺寸和
  4K 放大处理。因此原 V4L2 工作流目前只能按摄像头原始尺寸推流，不能直接恢复为 4K；
  需要后续独立的输出尺寸配置和 RGA 缩放链路。详细探针记录见
  `docs/4k/v4l2_4k_output_probe_20260714.md`。
- 已补齐可直接用于板端回归的 `runs/testsrc2/config_v4l2_720p.json`；视频源仍通过
  `source.type` 在 `v4l2` 和 `pull_stream` 之间切换。
- 2026-07-14 使用该配置完成约 60 秒板端 V4L2 推流测试：摄像头协商 1280x720 YUYV、实际
  10 FPS；MPP/RTMP 均为 1280x720，稳定窗口约 9.75～9.91 FPS，`put_fail/out_drop/write_fail`
  均为 0、队列深度为 0，检测持续运行并正常输出 `Done`。原始日志为
  `runs/testsrc2/board_v4l2_720p_20260714.log`。
- 2026-07-14 使用 `runs/input_videos/4k/VID_20260712_131410.mp4` 做 `pull_stream` 验证，
  输出地址复用 `rtmp://192.168.0.168/live/v4l2_720p_result`。板端成功解码
  3840x2160 yuvj420p，SRS 确认共享输出为活跃的 3840x2160 H.264 Main；稳定窗口约
  30.34～32.51 FPS、约 6.7 Mbps，另有窗口降至 26.30 FPS。稳定窗口无 put/out/write 错误，
  启动阶段发生 1 次 RTMP 重连和 12 个输出包丢弃后恢复。详细记录见
  `docs/4k/pull_stream_file_shared_output_20260714.md`。

## 2026-07-14 独立地址 4K 实际帧完整性验证

- 为排除复用 V4L2 测试地址和播放器旧分辨率状态的影响，新增配置
  `runs/testsrc2/config_pull_stream_unique_4k_20260714.json`，输出地址为
  `rtmp://192.168.0.168/live/pull_4k_verify_20260714`。
- 使用同一个 3840×2160 文件测试，板端解码日志、MPP/RTMP 元数据和 SRS API 均确认
  3840×2160；从独立地址抓取的 FLV 经 ffprobe 仍为 H.264 Main 3840×2160 30 FPS。
- 抽取实际输出帧后确认画面为完整 16:9 全幅，没有观察到左上角局部裁切。因此当前证据不支持
  “分辨率配置错误”或“4K 编码链路固定裁切”的结论；优先怀疑复用旧地址时播放器/SRS/解码器
  沿用旧的 1280×720 SPS/显示状态。详细记录见
  `docs/4k/pull_stream_unique_4k_frame_probe_20260714.md`。
- 后续复现必须先用独立地址或停止旧流后重新连接播放器；只有独立地址仍然出现裁切时，才继续
  排查播放器解码日志及 MPP buffer/stride 生命周期。

## 2026-07-14 4K 18 Mbps 推理推流验证

- 新增 18 Mbps 共享输出配置 `runs/testsrc2/config_pull_stream_18m_shared_output_20260714.json`，
  使用 `VID_20260712_131410.mp4` 进行 3840×2160 拉流、RKNN 推理、MPP 编码和 RTMP 转推。
- SRS 确认输入 3840×2160 H.264 High、接收约 18.03 Mbps；共享输出为 3840×2160 H.264 Main，
  发送约 16.10 Mbps，当前有 2 个客户端。
- 板端统计约 28.47～28.54 FPS、编码约 16.95 Mbps，`put_fail/out_drop/write_fail` 均为 0、
  队列为 0；RKNN 推理持续输出目标结果。详细记录见
  `docs/4k/4k_18m_inference_shared_output_20260714.md`。

## 2026-07-14 config.json 公共配置与差异配置

- `config/config.json` 保存公共的模型、检测、MQTT 和标签配置，并通过一个 `active_config`
  字段选择运行模式配置。
- `config/config_v4l2.json` 和 `config/config_4k_18mbps.json` 分别维护完整的模式差异，包括
  视频源、分辨率、帧率、码率、RTMP 地址和 Thumbnail/ROI/Tiling 图像处理开关。
- `src/main.cpp` 启动时按入口 JSON 所在目录加载并递归合并选中的差异 JSON；直接传入完整
  JSON 的旧方式仍兼容。统一启动命令为 `./AlertGateway config/config.json`。

## 2026-07-14 图像处理任务验证状态更正

- Thumbnail、ROI 和 ROI Tiling 的配置项及代码路径已经存在，但当前已确认的板端专项测试只覆盖
  基础 4K 拉流、推理、编码和推流链路。
- ROI 区域过滤、运动目标离开 ROI 的行为，以及 ROI + Tiling 对远距离小目标的识别提升尚未完成
  专项实测，不能在开发计划中标记为“已验证”。
- 当前 `config_4k_18mbps.json` 和 `config_v4l2.json` 中 Thumbnail/ROI/Tiling 均保持关闭，默认仍为
  全帧推理，不会自动选择这些模式。

## 2026-07-14 公共配置与 V4L2 选择器实测

- 使用新二进制和统一入口 `./AlertGateway config/config.json` 完成 V4L2 真机测试，启动日志确认
  加载 `config/config_v4l2.json`，公共配置和差异配置合并生效。
- 板端实际协商为 `/dev/video20`、1280×720 YUYV、10 FPS；MPP/RTMP 输出为 H.264 Main
  1280×720，SRS API 确认共享地址 `/live/v4l2_720p_result` 活跃，`put_fail/out_drop/write_fail`
  均为 0，推理持续运行。测试日志为 `/tmp/ag_v4l2_config_selector_20260714.log`。

## 2026-07-14 配置分辨率跟随实测

- 用户将板端 `config_v4l2.json` 改为 640×480 后，必须重启程序；运行中的进程不会热加载 JSON，未重启时日志仍显示旧的 1280×720。
- 重启统一入口 `./AlertGateway config/config.json` 后，日志确认 V4L2 实际协商 640×480 YUYV、`bytesperline=1280`、`sizeimage=614400`、15 FPS；MPP 编码初始化为 640×480，SRS 识别 H.264 Main 640×480。
- 640×480 RTMP 抓帧为完整单幅画面，无重复图像或额外填充；编码/推流约 15 FPS、约 3 Mbps，`put_fail=0`、`out_drop=0`、`write_fail=0`。测试日志 `/tmp/ag_640x480_config_test_20260714.log`。
- 本地 `config/config_v4l2.json` 已同步为 640×480；后续修改配置分辨率后仍需重启服务/进程。

## 2026-07-14 201 板 V4L2 720p 当前状态复核

- 201 板当前 `~/AlertGateway/config/config_v4l2.json` 已为 1280×720，运行进程通过统一入口
  `~/AlertGateway/AlertGateway ~/AlertGateway/config/config.json` 启动，并正确加载该差异配置。
- 板端日志确认 V4L2、MPP 编码和 RTMP 均为 1280×720；SRS API 与板端 ffprobe 对
  `/live/v4l2_720p_result` 均确认 H.264 Main 1280×720。因此当前未复现“配置已改但推流仍为
  640×480”；若修改后未重启，旧进程会继续使用启动时的 640×480 配置。
- 该摄像头在 1280×720 下实际只协商到 10 FPS，虽请求 15 FPS；这是独立的帧率协商问题，与分辨率无关。

## 2026-07-14 201 板 4K 18 Mbps 配置切换实测

- `config/config.json` 的 `active_config` 已切换为 `config_4k_18mbps.json`，该配置已部署到 201 板。
- WSL/主机使用 `VID_20260712_131410.mp4` 推送到 `rtmp://192.168.0.168/live/testsrc2`，201 板以
  `pull_stream` 拉取后完成 RKNN 推理、MPP 编码并输出到 `rtmp://192.168.0.168/live/v4l2_720p_result`。
- 201 板日志确认输入 3840×2160、MPP 输出 3840×2160、CBR 目标 18000 kbps；稳定窗口输出约
  30 FPS、约 17.9～18.2 Mbps，`put_fail/out_drop/write_fail` 均为 0，进程正常 `Done` 退出。
- 本次原始板端日志已保存为 `runs/testsrc2/board_4k_18mbps_active_20260714.log`。测试完成后板端
  AlertGateway 和 WSL 临时输入发布均已停止；本地入口配置仍保持 4K 选择状态。

## 2026-07-14 201 板 4K 循环查看状态

- 为便于人工查看，曾从 WSL/主机使用 `ffmpeg -re -stream_loop -1` 循环发布同一 4K 文件到
  `rtmp://192.168.0.168/live/testsrc2`，201 板使用 4K入口配置输出到
  `rtmp://192.168.0.168/live/v4l2_720p_result`。
- 循环查看日志再次确认 `active_config=config/config_4k_18mbps.json`、输入和 MPP 输出均为
  3840×2160，稳定窗口约 30 FPS、约 18 Mbps，`write_fail=0`；日志为
  `runs/testsrc2/board_4k_loop_20260714.log`。
- 本次查看会话被用户中断后，201 板端 AlertGateway 和 WSL 输入发布均已停止；本地
  `config/config.json` 仍保持 4K 选择状态。

## 2026-07-15 4K 检测框稳定化 SRS 测试

- 201 板 SSH 当日不可达，改用可访问的 200 板；未覆盖其原有配置，临时部署
  `AlertGateway.boxfix_20260715` 和 `config_boxfix_20260715.json`。
- 使用 `VID_20260712_131410.mp4` 经 SRS 发布 3840×2160 4K 输入，板端保持 30 FPS
  拉流、推理、MPP 编码并推送到独立地址 `pull_4k_verify_20260714`。
- SRS 确认输出为 H.264 Main、3840×2160，运行期间约 800 帧；板端稳定窗口
  `EncodeStats/StreamStats` 均约 30.01 FPS，编码平均处理约 17.7 ms，队列为 0，
  `put_fail=0`、`out_drop=0`、`write_fail=0`。
- 推理持续输出 1～4 个目标，未见 RKNN/编码/推流错误。该次测试确认新稳定化代码
  能在 4K 30 FPS SRS 链路正常运行，但因输出流在抓帧前结束，尚未完成人工画面级的
  “闪烁是否消失”确认；下一步应延长测试窗口并在流活跃期间抓取连续帧或播放器观察。

## 2026-07-15 4K 稳定化连续帧复核

- 五次循环测试启动后，在 SRS 输出 `pull_4k_verify_20260714` 抓取了连续帧联系图；
  整幅视频未观察到黑屏或 RTMP 断流，但绿色检测框的数量和位置仍有明显变化，说明
  剩余闪烁主要来自检测结果时序/模型抖动，而不是推流链路。
- 该次五循环测试在完成前被停止，不能作为完整五循环稳定性验收；板端临时测试进程和
  主机输入发布已停止。
- 当前 3 次推理保持约 130～150 ms，对 4K 30 FPS 运动画面仍偏短；下一步应改为基于
  时间的持久在线跟踪（约 500～800 ms）、连续命中确认和置信度滞回，避免仅靠简单 IoU
  平滑继续保留/创建不稳定框。

## 2026-07-15 4K 检测框时间保持与确认机制实现

- `InferThread` 已将检测框稳定化扩展为基于输入帧时间戳的保持机制：默认已确认目标
  最多保持 700 ms；不再受旧的 3 次推理上限截断。
- 新检测框默认需要连续命中 2 次才进入输出；已确认目标继续使用类别 + IoU 匹配和
  EMA 坐标平滑，降低短暂误检和单次漏检导致的闪烁。
- 公共配置新增 `detection_hold_ms=700` 与 `detection_confirm_hits=2`；4K 推流码率
  仍按用户要求使用 18 Mbps 配置。
- `cmake --build build -j2` 已再次通过；尚未用该最新二进制完成 18 Mbps 板端连续播放
  验证，下一步应部署后重新跑独立 SRS 地址测试。

## 2026-07-15 固定项目推流地址规则

- 用户明确要求本项目所有新的或当前有效的推理结果推流统一使用：
  `rtmp://192.168.0.168/live/alertgateway`。
- 该规则已写入根目录 `AGENTS.md`；`config/config_4k_18mbps.json` 和
  `config/config_v4l2.json` 已同步。历史实验配置和报告中的旧地址保留为历史证据，
  不作为后续运行配置使用。

## 2026-07-15 检测框稳定化方案撤回

- 用户反馈 700 ms 时间保持、连续命中确认、IoU/EMA 平滑会在快速移动画面中产生明显
  延迟框；该整套检测框保持/跟踪/时间戳选框改动已撤回。
- `InferThread`、`SharedDetections` 和 `EncodeThread` 已恢复为原始行为：推理线程直接
  发布最新检测结果，编码线程直接读取最新结果叠框；`config.json` 中相关稳定化参数也已删除。
- 撤回后的 `cmake --build build -j2` 已通过。固定 RTMP 输出地址规则和 18 Mbps 4K 配置
  仍然保留。

## 2026-07-15 检测框相邻结果插值实现

- 根据用户建议，`SharedDetections` 现在保存相邻两次带时间戳的检测结果；
  `EncodeThread` 按当前编码帧时间戳，在两组结果之间对同类别且 IoU≥0.20 的框做线性
  坐标插值。
- 插值不保持未匹配或消失目标，避免快速移动时出现延迟框；旧 `get()` 仅保留给 smoke
  调试调用，生产叠框使用 `get_interpolated()`。
- 最新 `cmake --build build -j2` 已通过，尚未完成板端 18 Mbps 固定地址实测。

## 2026-07-15 4K 检测框抗闪烁初步实现

- 按用户要求保留 4K 30 FPS，不调整输入/输出帧率。
- `ModelConfig` 新增检测结果稳定化参数：`detection_hold_frames=3`、
  `detection_match_iou=0.30`、`detection_smoothing_alpha=0.30`；参数位于公共
  `config/config.json`，4K/V4L2 差异配置通过合并逻辑继承。
- `InferThread` 已加入按类别和 IoU 的检测框匹配、EMA 坐标平滑，以及单个目标最多
  3 次推理周期的漏检保持，避免一次漏检立即清除边框。
- `SharedDetections` 已保存最近 8 次带输入帧时间戳的检测快照；`EncodeThread` 按
  当前视频帧时间戳选择最近结果，避免无条件使用最新但可能属于未来/过旧输入帧的框。
- 交叉编译 `cmake --build build -j2` 已通过，AlertGateway、infer_camera_smoke 和
  image_processing_smoke 均成功构建；尚未完成 201 板实际 4K 人工播放对照，下一步应
  在独立 RTMP 地址运行并观察边框闪烁是否消失，再按结果调整保持帧数/IoU/平滑系数。

## 2026-07-15 4K 最新帧轨迹预测方案与三循环验证

- 用户要求撤销相邻结果插值后，已按 `docs/4k/4K 闪烁解决计划.md` 实施低依赖轨迹方案：
  推理队列改为单槽最新帧覆盖；帧携带 `frame_id/pts_ms`；检测框按类别、IoU、中心距离关联；
  连续命中 2 次才显示，单次漏检保留 300ms；编码端按当前 PTS 做速度预测和自适应 EMA，
  不等待下一轮推理。
- 原始“无时间对齐直接画最近结果”的实现是闪烁/延迟根因之一：旧队列满时会丢新帧，编码端
  也无法判断检测结果所属视频帧。当前 `push_latest()` 会明确覆盖旧推理任务；编码和 RTMP
  链路保持不变。
- 已在 200 板完成 3 次 4K 视频循环的 195 秒验证，固定输出始终为
  `rtmp://192.168.0.168/live/alertgateway`、3840×2160、30FPS、18Mbps。稳定窗口约 30FPS、
  17.85--18.24Mbps，`put_fail/out_drop/write_fail=0`；推理队列仅 0/1，约 79--91 个旧推理帧
  每 10 秒被替换，显示框 `pts_delta_ms` 稳定约 62--98ms，未累积。程序正常 `Done` 退出。
- 输出连续帧抽样未见插值式拖尾；但测试视频场景切换较快，最终“肉眼闪烁是否足够改善”仍应
  由用户在播放器持续观察后决定。详细实现、参数和日志判读已追加到上述计划文档。

## 2026-07-15 残影诊断与 Active/Shadow 分离

- 用户反馈快速运动时框残影后，已定位此前 Mode 2 直接从原始 YOLO 框计算速度并外推会放大
  检测坐标噪声；`pts_ms` 均来自同一 `steady_clock` 毫秒基准，完整 Track 快照在锁内读取，
  未发现时间基准混用或部分字段竞争。
- `SharedDetections` 已加入 Tentative/Active/Shadow 状态：Tentative 不显示；检测失配的
  Active 立即转 Shadow，Shadow 只保留 300ms 用于关联恢复且永不绘制；未接入视觉跟踪时
  Active 显示结果最多保留 100ms。新检测先关联 Active/Shadow，再做重复框抑制后才新建。
- 已完成 Mode 1 有效 4K 诊断：日志按编码帧输出实际绘制的 Active `trackId/坐标/数量`，
  并保存 `/tmp/ag_overlay_before_[0-2].pgm` 与 `after` 配对帧。每帧 MPP buffer 都先由当前
  NV12 解码帧完整拷贝/RGA 转换再叠框，代码和诊断均未见复用上一帧已叠框像素的路径。
- 交叉 sysroot 有 OpenCV 头文件但未发现 ARM64 `opencv_core/imgproc/video` 库，不能直接链接
  `calcOpticalFlowPyrLK`。后续 Mode 3 若继续，应实现项目内低分辨率轻量 LK，或先补齐同版本
  ARM64 OpenCV 库；不可只加入头文件或引入不匹配的大型依赖。

## 2026-07-15 Mode 1 小幅稳定性微调

- 用户确认当前回退后的 Mode 1 在小幅运动时框准确性和闪烁均有改善，但仍有轻微抖动。
  为不牺牲快速运动跟随，先只将 `track_deadzone_center_px` 从 6.0 调至 8.0 个 4K 像素；
  `track_ema_alpha=0.25`、确认次数、300ms TTL、帧率及码率均未改变。
- 后续应在相同输入下循环 3 次观察；若仍有坐标噪声且准确性不退化，下一步只考虑 8→10，
  不同时修改 EMA 或 TTL。

## 2026-07-15 8 像素死区测试撤回

- 三循环观看测试在启动后稳定至约 30FPS、18.10Mbps、零编码/推流失败，但用户观察到快速
  移动时边框残留更明显，且框的变化幅度可能大于实际目标运动。
- 当前确认运行的是 Mode 1，不存在速度预测；8 像素死区会冻结真实的小位移，后续校正时
  形成更明显的跳变/残留感。因此该单变量微调已撤回，`track_deadzone_center_px` 恢复为 6.0。
- 本轮测试已停止，不把 8 像素版本作为候选。后续应优先将“显示保持时间”从内部轨迹保留
  中分离，缩短旧框实际绘制时间，而不是继续扩大静止死区。

## 2026-07-15 大幅位移异常校正门限

- 用户确认恢复到 6 像素死区后，慢速移动显示可接受，但大幅移动时框的变化幅度仍可能超出
  实际体验。当前 Mode 1 没有速度外推，因此优先防护单次 YOLO 偏差或错误关联，而不调整
  EMA/TTL。
- 新增 `track_innovation_iou=0.45`、`track_max_correction_px=100` 和
  `track_jump_confirm_hits=2`：中心位移超过 100 像素且 IoU 低于 0.45 的匹配先作为候选，
  需连续两次位置一致才接受；接受后单次显示中心校正仍限幅为 100 像素。慢速正常检测不走
  此门限。`cmake --build build -j2` 已通过，尚未完成板端实测。

## 2026-07-15 检测时间对齐视频微缓冲

- 用户反馈当前 Mode 1 仍有明显相对画面滞后。根因是 30FPS 视频立即编码而单次 YOLO 约
  40ms，框天然晚于当前画面；继续增加 EMA 或外推均不能同时兼顾贴合与稳定。
- 已实现不改变帧率的 2 帧编码微缓冲（约 66ms），并在 `SharedDetections` 保存 12 个带源帧
  PTS 的检测快照。编码叠框只选择 PTS 不晚于待编码视频帧的最近检测快照，不进行插值或
  速度外推。公共配置新增 `track_align_to_video_pts=true` 和
  `stream.detection_alignment_delay_frames=2`。
- `cmake --build build -j2`、附带 smoke 目标和 JSON 校验均通过，尚未完成板端实测。

- 首次 2 帧板端实测稳定后仍为 `pts_delta_ms=91--103ms`，说明实际推理完成落后接近 3 个
  视频帧；对齐延迟已调整为 3 帧（约 100ms），待重新部署复测。帧率与码率不变。

- 用户要求进一步减小快速移动时显示框的单次变化幅度，`track_max_correction_px` 从 100.0
  下调至 80.0；异常二次确认和其它 Mode 1 参数保持不变，待板端复测。

- 80 像素限幅复测后，用户仍认为幅度偏大；日志表明多数 EMA 校正本来低于该上限，根因是
  常规 `track_ema_alpha=0.25`。新增自适应规则：中心距离大于 60 像素时改用
  `track_large_motion_ema_alpha=0.10`，其余保持 0.25；这样压低大幅移动的单次校正，慢速
  跟随行为不变。待重新构建/板端验证。

## 2026-07-15 Mode 3 实验候选：低分辨率稀疏 LK 视觉跟踪（未验收）

- 新增 `src/encode/VisualTracker.hpp`，不引入 OpenCV 或大型第三方框架。Mode 3 在编码线程中
  从当前 `Frame::raw_data` 的亮度面生成 640×360 灰度帧；每个 Active 框提取最多 24 个高梯度
  稀疏点，以前后向 LK 光流的有效点中位数更新中心位置。YOLO 新快照到达时重新关联、校正框并
  重取点；宽高继续由 YOLO/原有 EMA 管理，光流不外推框尺寸。
- 视觉轨迹有明确 Active/Shadow 分离：只有 Active 进入绘制列表；有效点少于 8 个或本次检测
  未关联时立即转 Shadow，不显示旧框；Shadow 仅保留 300ms 用于同类别、IoU/中心距离匹配的
  恢复，随后删除。这样 TTL 不再意味着旧框持续显示，也避免同一目标新旧框同时绘制。
- Mode 3 不改动 `InferThread`、`SharedDetections` 的锁边界、MPP/RGA 缓冲复用或 RTMP 地址；
  编码线程仍在每帧先将当前原始解码图写入 DRM buffer，再叠框。视觉灰度输入始终来自原始帧，
  不会追踪上一帧已经叠过框的 MPP buffer。Mode 0/1/2 保持可选，根配置默认仍为 Mode 1；
  专用 4K 测试配置使用 Mode 3、`640×360` 跟踪分辨率。
- 初版板端暴露出同一检测批次新建多个轨迹时 `used` 关联数组未同步扩容的越界访问；已定位为
  `VisualTracker::apply_detections`，修复为新建轨迹同时标记已匹配后重新构建并部署。
- `cmake --build build -j2` 通过。200 板进行功能性测试，使用固定输入 `/live/testsrc2`、固定输出
  `rtmp://192.168.0.168/live/alertgateway`、3840×2160/30FPS/18Mbps，运行至约 3,561 帧并
  正常 `Done`；稳定窗口编码/输出约 28.95--30.15 FPS、约 17.3--18.25 Mbps、平均编码处理
  约 20.3--21.9ms，`put_fail/out_drop/write_fail=0`，未再发生段错误。日志显示 Active/Shadow
  数量随检测变化切换。
- 这只证明“代码可构建、Mode 3 不会立即崩溃、4K 编码/推流链路仍可运行”，不构成检测框质量
  验收，也不代表残影、拖尾、快速移动准确性、端到端延迟或闪烁已解决。当前默认仍必须使用
  Mode 1；Mode 3 只能作为后续 A/B 诊断候选，待播放器人工对照和问题定位后再决定是否保留。

## 2026-07-15 Mode 1 快速移动跟随延迟修正（待视觉验收）

- 实流日志确认此前约 0.3 秒跟随延迟的主要人为来源：中心误差达到约 100～239 个 4K 像素时，
  `track_large_motion_ema_alpha=0.10` 每次只吸收 10% 新测量；80 像素校正上限和 Active 轨迹的
  异常位移二次确认又进一步延后更新。这不是 30FPS 或 RTMP 写入错误。
- Mode 1 小运动仍使用 `track_ema_alpha=0.25`；大运动系数调整为 0.55，中心校正上限调整为
  120 像素。Active 轨迹的大位移现在立即进行有限校正，不再冻结一轮；Tentative/Shadow 的
  建立和恢复仍保留二次确认。Mode 1 仍不做速度外推，因此不会预测越过当前 YOLO 测量框。
- 同时将内部 300ms 轨迹 TTL 与显示保留分开：Active 短漏检最多显示 100ms，之后转 Shadow
  隐藏但保留关联；快速移动关联允许在尺寸相近时使用受限中心距离恢复，减少因无 IoU 而重建。
- 交叉构建、JSON 校验和本地生命周期自测通过。200 板最终实流约 1,784 帧，固定输出
  `rtmp://192.168.0.168/live/alertgateway`、3840×2160、30FPS、18Mbps；稳定窗口约
  29.49～29.98 FPS、17.65～18.08Mbps，`put_fail/out_drop/write_fail=0`，输入结束后正常
  `Done`。日志确认 Active 大位移更新为 `pending_hits=0` 且当次修正，常见 PTS 差为 0～49ms。
- 以上证明参数和状态逻辑生效、链路稳定，但不等于肉眼延迟/闪烁已完全验收；下一步只需在
  相同片段观察跟随和残影，若仍有明显延迟，应先区分 Shadow/Tentative 恢复与 Active 跟随，
  不再降低大运动 EMA 系数。

## 2026-07-15 4K 检测框线宽 A/B（待视觉结论）

- 用户观察到移动过程仍有闪烁，当前绘制代码确认所有分辨率原先统一使用 2 像素边框；在
  3840×2160 画面中该线宽偏细，可能放大几像素坐标变化的视觉闪烁感，但不是检测时序抖动本身。
- 已做单变量修改：输出分辨率达到 3840×2160 或更高时使用 6 像素边框，低于 4K 仍保持
  2 像素；跟踪、确认、EMA、显示保持、3 帧对齐、30FPS 和 18Mbps 参数均未改变。
- 交叉构建通过，候选 `AlertGateway.box6_4k_20260715` 已部署到 200 板。SRS 确认输入和固定输出
  `rtmp://192.168.0.168/live/alertgateway` 均为 3840×2160，候选测试流已启动；是否降低肉眼
  闪烁仍等待用户播放器观察，不提前标记为验收通过。

## 2026-07-15 Mode 1 连续自适应滤波（待视觉验收）

- 固定 `0.25/0.55` 两档 EMA 已替换为按目标尺寸和实际 PTS 间隔归一化的连续自适应滤波；
  中心权重范围 0.18～0.90，宽高独立权重范围 0.12～0.45，快速移动时中心高响应、框尺寸仍
  强抑噪。Mode 1 不做预测/外推，确认、TTL、100ms 显示保持和三帧视频对齐均未增加。
- Tentative/Shadow 恢复或低 IoU 距离关联会重置到当前检测，避免旧框缓慢追赶；新增日志字段
  `center_motion/size_motion/center_alpha/size_alpha` 用于判读实际响应。
- 全部交叉构建目标、JSON 解析和差异检查通过；候选 `AlertGateway.adaptive_20260715` 已部署
  200 板，以 3840×2160/30FPS/18Mbps、4K 6 像素边框推送固定地址。初始稳定窗口约
  29.9FPS/18.0Mbps、错误计数为 0，主观抖动与移动延迟仍需用户观察后决定是否接受。

- 用户完成肉眼观察后确认：画面移动时检测框抖动仍明显，当前自适应滤波不能作为最终验收结果。
  10 分钟日志共约 46,364 次 TrackUpdate，其中新建轨迹 2,008 次、`center_alpha=1` 的直接重置
  3,632 次；约 7.8% 更新绕过平滑。移动阶段自适应中心权重已达到约 0.55～0.84，说明继续降低
  EMA/自适应权重会增加跟随延迟，主要问题应转向移动时的轨迹关联连续性。
- 下一候选应只为“检测关联”加入受限速度预测：用相邻原始检测中心估计、限幅并低通过滤速度，
  将轨迹框预测到下一次检测 PTS 后参与 IoU/中心距离匹配；该预测不得进入绘制框。目标是减少
  Track 重建和 `alpha=1` 重置，同时保持当前显示滤波及三帧 PTS 对齐，不增加显示延迟。
- 该候选的正式实施计划已写入 `docs/4k/4K 闪烁解决计划.md`：包括算法边界、建议配置、修改
  文件、日志字段、同源 A/B 指标和回退条件。当前仅完成文档规划，尚未修改关联预测代码。

## 2026-07-15 未提交代码整理

- 已移除未通过视觉验收的 Mode 2 显示速度外推和 Mode 3 稀疏 LK 生产路径；未跟踪的
  `src/encode/VisualTracker.hpp` 已删除。`track_display_mode` 仅允许 0（原始）或 1（自适应），
  非法值启动失败；下一阶段关联专用预测仍只存在于计划文档，尚未实现。
- 删除 Mode 2 专属速度/预测配置。逐框 TrackUpdate 和周期 TrackStats 改由
  `track_debug_logging` 控制，公共配置默认 false，隔离 4K A/B 配置为 true。
- `.gitignore` 已忽略 `runs/` 下训练输出、输入/抓取视频、日志和大型画质对比目录；本地文件
  未删除。保留所有来源不明的用户修改、实验文档、小型配置和仍可复现的脚本；仅删除了未跟踪、
  无引用且仍使用旧 `/live/testsrc2_result` 输出的 `runs/testsrc2/test_manual.sh`。

## 2026-07-15 关联专用受限速度预测实现（待板端 A/B）

- 已实现上一阶段计划中的关联专用速度预测：Active Track 使用相邻原始检测和真实 PTS 估速，
  经 EMA、目标对角线归一化限速后，只把最后原始检测框推进到下一检测 PTS 参与关联。预测时间
  上限 120ms、位移上限为框对角线 25%，宽高不预测。
- 预测框绝不进入绘制；Mode 1 显示仍使用原有连续自适应滤波，EncodeThread、三帧 PTS 对齐、
  MPP 和固定 RTMP 输出均未修改。公共配置默认关闭，隔离 4K 配置开启，可用同一二进制回退。
- 新增关联速度、预测位移/限幅、预测命中、拒绝原因及累计 TrackStats 日志。交叉构建、JSON、
  diff 检查及主机合成轨迹自测通过；自测确认预测可避免一次 Track 重建且不会直接推进显示框。
- 200 板约 80 秒短时 Smoke 已通过：稳定窗口约 29.97～30.08FPS、18.0～18.18Mbps，
  `put_fail/out_drop/write_fail=0`，正常退出；记录 5,704 次可信速度更新、39 次预测限幅。但本次
  `matched_by_prediction=1` 为 0，尚无直接救回关联的实流证据，不能据此认定抖动改善。
- 尚未完成同源关闭/开启 A/B 和人工视觉验收；下一步应比较 Track 新建与 `center_alpha=1` 比例
  是否下降至少 30%，若未下降则回退或修正关联预测，不扩大显示 TTL。

- 用户完成连续观看后确认：边框仍有抖动，跟随延迟可以接受，暂未看到明显错框。本轮约
  17,020 次 TrackUpdate、736 次新建、1,315 次 `center_alpha=1`，15,508 次可信速度更新、
  91 次预测限幅，但 `matched_by_prediction=1` 仍为 0；稳定窗口约 30FPS/18Mbps，错误计数为 0，
  正常退出。
- 当前关联预测没有直接救回实流匹配，不能视为有效的抖动优化；不要扩大预测时间、位移、匹配
  距离或显示 TTL。下一步应保持当前可接受的跟随速度，转向同一 Active Track 内的中心/尺寸
  测量噪声和 `center_alpha=1` 重置路径，设计可关闭的低延迟单变量 A/B。

## 2026-07-15 方向反转阻尼候选（待板端视觉验收）

- 取消 Active 低 IoU 直接重置的试验在 80 秒 Smoke 中触发 0 次，已撤回。204 次
  `center_alpha=1` 中 109 次为新 Track、92 次主要为 Tentative 首次确认显示、3 次为异常确认，
  因此持续显示抖动主要应看 Active Track 内的测量方向反转，而非笼统统计全部 alpha=1。
- 已增加可关闭的方向反转阻尼：相邻原始中心位移点积小于 0 且位移超过框对角线 0.5% 时，
  本次中心 alpha 最大 0.35；持续同向移动和尺寸滤波不变。公共配置默认关闭，4K 测试配置开启，
  并关闭无实流收益的关联预测以保持单变量。
- 交叉构建、JSON、diff 检查及合成往返抖动自测通过。200 板 60 秒 Smoke 累计触发 237 次反转
  阻尼，稳定窗口约 29.77～30.14FPS、17.89～18.14Mbps，`put_fail/out_drop/write_fail=0`，正常
  退出；候选确实介入实流，尚待固定地址人工观看抖动和跟随延迟。
- 用户观看确认大幅来回摆动减少，跟随延迟可接受且未见明显错框，但仍有小幅抖动。随后已从
  活动源码和配置删除两轮实流均为零直接命中的关联预测整条路径（6 个配置、速度状态、候选、
  统计与日志）；零触发的 Active 低 IoU 试验也未保留。历史结果仅保留在文档。
- 清理后活动候选只包含方向反转阻尼；交叉构建、JSON、diff 检查和合成往返测试再次通过。
  下一步先复测清理版，再考虑只在稳定低速状态生效、带残差累计的尺寸归一化微死区。

## 2026-07-16 未推送基线代码清理

- 删除 `SharedDetections::Track::last_frame_id`：该字段只写入、没有读取路径。
- 删除无仓库引用的历史 4K 临时配置与执行脚本；保留当前 4K/V4L2 基线及仍被文档引用的对比资产。
- 修正 MPP Level/GOP 的过期分辨率、帧率注释；不改变编码参数或跟踪算法。

## 2026-07-16 全局中心运动诊断与对齐 A/B

- `SharedDetections` 新增默认关闭的 `track_motion_stats_logging`。诊断只统计连续两次都存在的
  相同 Active `trackId` 集合，输出 raw/smooth 稳健中心、中心位移差、PTS 间隔和原始方向反转数；
  不改变叠框、轨迹关联、MPP 或 RTMP 行为。临时诊断配置保留在 `runs/testsrc2/`。
- 交叉编译、JSON 校验和 `git diff --check` 通过。200 板短时 4K 回归确认诊断版本在稳定窗口约
  29.8--30.2 FPS、约 18 Mbps 下运行，`put_fail=0`、`write_fail=0`、队列为 0；启动阶段曾出现
  短暂输出丢包，稳定窗口恢复为 0。
- 连续 `trackId` 统计显示多数窗口 raw/smooth 中心差约 8--30 个 4K 像素，快速变化窗口可达
  约 46--60 个；部分窗口存在原始位移方向反转，说明可以用该统计区分真实整体运动、滤波滞后和
  检测抖动，但不能再直接对随目标数量变化的全部框取中心。
- 完成非严格起点的 3 帧/2 帧对齐短时 A/B：2 帧后半段约 30 FPS、18 Mbps、`pts_delta_ms`
  约 0--24 ms，但统计中仍出现较大 `delta_gap`，且两次测试未从同一源帧开始，暂不足以替换
  生产 3 帧配置。最新二进制和生产配置已恢复运行，固定输出地址不变。
- 下一步若继续降低端到端延迟，必须用同一源片段、同一起点严格比较 3 帧与 2 帧；先不重新启用
  速度外推、插值或 LK 生产路径。

## 2026-07-16 检测框量化工具与尺寸滤波 A/B

- `track_motion_stats_logging` 诊断扩展为逐次输出连续 Active Track 集合的稳健原始/显示中心、
  中位尺寸、归一化中心/尺寸步长、方向反转、轨迹 ID 及新建/丢失数量；默认关闭，不改变绘制。
- 新增 `tools/analysis/analyze_motion_stats.py` 及标准库单元测试。工具按整体中心速度区分低速和
  运动区间，报告中心/尺寸抖动、单步跳变、等效滞后、轨迹连续性、编码 FPS/码率/PTS 和失败
  计数，并支持两个日志直接给出改善/退化百分比；合成测试、交叉构建和 JSON/diff 检查通过。
- 首轮 67 秒基线共 1320 个有效样本：低速中心抖动 RMS 为框对角线 0.74%，相对原始框降低
  19.29%；低速尺寸抖动 RMS 为 1.27%，降低 43.60%；运动等效滞后 P90 为 39.55ms。
- 完成 `track_size_alpha_max=0.45/0.30` 单变量同源起点 A/B。候选低速尺寸抖动改善 21.34%，
  全段单步跳变 P95 改善约 14.6%，但运动跳变 P90 退化 7.24%、等效滞后 P90 退化 18.03%。
  两组运动样本数相差 17.2%，编码输入丢帧累计 12/27，说明 30FPS 最新帧覆盖使两次运行选择
  的推理帧不同；候选证据不足，未部署为活动版本。详细记录见
  `docs/4k/4k_detection_box_motion_metrics_20260716.md` 和 `runs/testsrc2/motion_ab_size30_20260716.*`。
- 已从 Git `f3970aa` 隔离构建 Release 严格提交版并部署，板端二进制 SHA-256 为
  `f84867038422d25bda9140962ee94fcc2390cb69f2341e54526d00dc710cb60b`；提交版
  `config/config.json` 与 `config_4k_18mbps.json` 同步恢复。4K 循环输入和固定输出
  `rtmp://192.168.0.168/live/alertgateway` 均处于活动状态，推理总耗时恢复约 38--49ms。
  下一步先记录一次逐 Track 原始测量，离线重放相同序列比较 size alpha 0.45/0.35/0.30，
  再只把唯一胜出参数上板人工观看。

## 2026-07-16 中心运动门控尺寸滤波候选（活动观看中）

- `[MotionTrack]` 诊断已能记录同一 Active Track 的原始框、生产平滑框、PTS、死区/重置决策、
  中心/尺寸 alpha 和有效尺寸上限。新增离线重放器及单元测试；5570 个样本上对生产 max=0.45
  尺寸路径的 P99 复刻误差为 0.0005px、最大 0.0008px，严格低于 0.10px 门限。
- 同序列固定 alpha 扫描中，没有 0.42/0.40/0.38/0.35/0.30 候选同时满足低速边缘抖动改善至少
  5% 且运动边缘误差增加不超过 10%，因此拒绝全局降低尺寸 alpha 上限。
- 新增默认关闭的中心运动门控尺寸滤波：低速尺寸 alpha 上限为 0.30，随既有连续中心运动响应
  平滑恢复到 0.45。离线结果相对基线：低速边缘 RMS 改善约 10.7%，全段边缘 P95 改善约 6.1%，
  运动边缘误差 P90 增加约 3.6%，运动边缘步长 P90 基本不变。
- 交叉构建、JSON、两套分析单元测试和 diff 检查通过。板端 75 秒 Smoke 正常结束，稳定窗口约
  28.39--31.05FPS、4K/18Mbps，`put_fail/out_drop/write_fail=0`，日志确认有效尺寸上限连续变化。
- 后续同序列把低速上限细扫到 0.12：0.18 是运动边缘误差增加不超过 10% 的最后参数，0.16
  开始越线。为保留跟随余量，活动候选选择 0.20；相对 0.45 基线，低速边缘 RMS 改善约 18.6%，
  全段边缘 P95 改善约 12.3%，运动边缘误差 P90 增加约 6.8%，运动边缘步长 P90 增加约 1.4%。
- size20 板端稳定窗口约 29.74--30.16FPS、约 17.8--18.2Mbps，失败/丢弃计数为 0。录制的
  20.01 秒固定输出共 595 帧、约 17.87Mbps，全段可解码，未检出超过 250ms 的冻结或黑帧；
  1FPS/10FPS 抽帧初验未见尺寸门控引入突然缩放，目标框出现/消失仍可能由检测置信度导致。
- 当前 200 板活动程序 SHA-256 为
  `4d43c5d9ddf8f9dbaf8637d057ac8e098017c36c33ef15238f505f2737da7ac9`，使用关闭逐 Track 诊断的
  `config_4k_center_gated_size20_view_20260716.json` 持续推送固定输出；严格 `f3970aa` 基线程序
  `AlertGateway.f3970aa_release` 保留在板端，可随时恢复。公共配置仍默认关闭门控，当前候选仅待
  用户人工视觉判断，尚未提交或推送 Git。

## 2026-07-16 全局共同中心位移候选（活动观看中）

- 用户确认 size20 尺寸门控后慢速移动仍抖。已有逐 Track 数据表明慢速硬死区占比仅 3.2%、
  释放步长 P90 约 2.75px，不是主因；慢速中心 alpha 的 P50/P90 仍为 0.243/0.366。
- 单纯压低瞬时中心响应会显著增加快速运动误差，已拒绝。新候选只在同帧至少三个连续 Active
  Track 时，使用原始中心位移 x/y 中位数的 0.25 EMA 作为当前帧共同位移前馈，再由原滤波处理
  每个框的残差；不预测未来位置，目标不足时自动回退原路径，公共配置默认关闭。
- 5570 样本离线重放相对原中心路径：低速中心粗糙度 RMS 改善约 9.9%，低速中心误差 P90 改善
  约 5.1%，运动中心误差 P90 改善约 26.0%，全段中心步长 P95 改善约 1.8%，运动步长 P90 仅
  增加约 1.1%。baseline 中心重放 P99/最大误差为 0.0004/0.0006px。
- 板端 65 秒 Smoke 正常结束，4956 个逐 Track 样本中共同位移实际应用 3951 次（79.7%）；编码
  FPS/码率 P50 为 29.98/18.03Mbps，失败计数为 0。20.01 秒持续流录制共 597 帧、约 17.97Mbps，
  全段解码且未检出超过 250ms 的冻结或黑帧。
- 当前 200 板活动二进制 SHA-256 为
  `78fa930072390cc32289f143e7668ec7d2970c64b918445f50b93ad105a0d345`，使用关闭逐 Track 诊断的
  `config_4k_global_motion_size20_view_20260716.json` 推送固定输出。严格 `f3970aa` 基线仍保留，
  当前候选等待用户观看慢速平移后决定保留或回退，尚未提交或推送 Git。

## 2026-07-17 全局共同位移一致性门控候选（已拒绝，代码未保留）

- 曾尝试用一致性门控限制共同位移：按各框对角线筛除偏离中位位移过大的 Track，达到 67% 一致
  比例才应用共同位移。该变体不预测未来位置。
- 在同一 `/tmp/ag_motion_replay_p9_20260716.log` 的 5570 条样本上，最小一致比例 0.67、
  最大残差 0.04 倍框对角线：低速中心粗糙度 RMS 由 0.766% 降至 0.703%，运动中心误差 P90
  由 1.533% 降至 1.149%，运动中心步长 P90 由 8.521% 降至 8.479%；该离线收益不足以代表主观
  稳定性。
- 板端候选 Smoke 已完成：输入 `rtmp://192.168.0.168/live/testsrc2` 成功以 3840×2160/30FPS 拉流，
  稳定窗口编码/输出约 29.43--30.04 FPS、17.72--18.07Mbps，平均编码处理约 18.75--19.16ms，
  `put_fail/out_drop/write_fail=0`，最终正常 `Done`。本次观看配置关闭逐 Track 诊断，故该 Smoke
  只证明稳定性，不量化一致性门控的实际覆盖率，也未完成主观视觉验收。
- 人工观看结论：一致性门控候选的检测框晃动幅度更大、观感更不稳定，已拒绝。该结果与离线重放
  中低速单步位移 RMS 增加的风险一致；不要提高共同位移权重、放宽一致性阈值或继续在此分支调参。
  一致性门控代码和配置已从工作区移除。随后关闭共同位移的 size20 对照也被用户观察为晃动幅度
  偏大；当前只将此前已通过大幅往返摆动人工观察的方向反转阻尼作为回归对照，固定输出地址不变。

## 2026-07-17 4K 识别准确率数据闭环（待执行）

- 用户报告真实 4K 场景中 `cup`、`book`（含说明书）和 `laptop` 的识别率、检测框贴合度偏低。
  当前决定先复用已有 4K 测试视频，以 1 FPS 抽帧、筛选和人工标注建立数据集，再按完整视频片段
  划分 train/val/test，避免相邻帧跨集合造成评估泄漏。
- 本轮保持既有六类别定义，说明书统一标为 `book`；若要把说明书作为独立类别，必须另行决定类别
  契约并重新收集、训练和评估。
- 微调在主机 WSL/Linux GPU 环境完成；只有浮点模型在独立 4K test 集逐类验证确有提升后，才从
  train/val 中选取代表帧做 INT8 校准并生成 RKNN。校准不替代训练，不能直接提高识别率或修正框。
- 执行指南：`docs/4k/4k_accuracy_finetune_and_int8_calibration.md`；已从
  `docs/4k/README.md` 和仓库根 `readme.md` 建立入口。当前尚未开始抽帧、标注、训练或转换。

### 抽帧完成（待筛选与标注）

- 已从 `runs/input_videos/4k/` 的 5 段现有 3840×2160/30FPS 测试视频按 1 FPS 抽取候选帧，
  原始帧保存在仓库外 `/home/rambos/datasets/alertgateway_4k_refine/raw/<video_id>/`，不得修改。
  各视频帧数：`VID_20260712_131214` 83、`VID_20260712_131410` 67、
  `VID_20260712_131553` 63、`VID_20260716_112619` 31、`VID_20260716_112700` 30；合计 274 张，
  约 162 MB。
- 下一步：人工筛除重复/模糊帧，将保留帧复制到
  `/home/rambos/datasets/alertgateway_4k_refine/annotation/images/`，创建六类 `classes.txt` 后用
  `tools/dataset/run_yolo_annotator.py --annotation-dir .../annotation` 标注；尚未创建训练集、未训练、
  未做 INT8 校准。
- 标注工作区已初始化于 `/home/rambos/datasets/alertgateway_4k_refine/annotation/`：`classes.txt`
  固定为现有六类顺序，`images/` 已从 `raw/` 创建 274 张硬链接，`labels/` 当前为空。标注器中删除
  或移动 `annotation/images/` 的图片不会修改 `raw/` 原始帧；下一步由用户在浏览器筛选/标注。
- 已增强 `tools/dataset/run_yolo_annotator.py` 的 4K 标注交互：选中框后可拖框内整体移动，拖黄色
  控制点调整四角/四边，并有放大、缩小、适应窗口按钮；Python/前端 JavaScript 语法检查和针对该
  工作区的短时服务启动检查通过。尚未产生人工标签。
- 已用现有 `/home/rambos/yolov8_models/onnx/yolov8s_official.onnx` 通过
  `tools/dataset/pseudo_label_desktop6.py` 对 274 张 4K 标注候选帧做预标注（ONNX Runtime，
  置信度阈值 0.30，未覆盖人工标签）。输出位于 `annotation/labels/` 和
  `annotation/pseudo_label_manifest.csv`：206 张有候选标签、68 张无标签文件（不等同负样本，
  必须人工判断）；候选框数为手机 28、杯子 17、键盘 117、鼠标 122、笔记本 99、书本 73。
  `tvmonitor` 的已有映射会归入 `laptop`，所以笔记本候选必须人工核对。下一步优先核对 cup/book/
  laptop 和无候选帧，修正/补框后保存为人工标签。
- 用户决定本轮只标注项目最常用的 4K 回放源 `VID_20260712_131410.mp4`。活动标注目录
  `annotation/images/` 已仅保留其 67 张（`00001`--`00067`），因此重启标注器后列表从 1 到 67；
  其中 60 张已有预标注。其他 204 张当前候选图片和 146 个预标注标签已移动到可恢复的
  `annotation/excluded/images|labels/`，未修改 `raw/` 原始帧，也未碰触 `annotation/deleted/`。
- 用户已完成该 67 张的人工核对/标注。只读校验结果：67 张图片与 67 个标签一一对应、无空标签、
  无非法 YOLO 行或坐标范围错误；共 310 个框（手机 27、杯子 52、键盘 48、鼠标 52、笔记本 30、
  书本 101）。这批数据可作为 4K 微调训练来源，但因全部来自同一连续视频，不能同时作为独立
  test 集；下一步必须从另一整段视频建立独立测试集，或补充更多不同场景的视频后再按视频划分。
- 在用户暂停继续标注后，已对这 67 张运行当前官方 ONNX 基线（`yolov8s_official.onnx`、
  置信度 0.30、NMS IoU 0.45），预测写入隔离目录
  `/home/rambos/datasets/alertgateway_4k_refine/baseline_predictions_131410/`，未改动人工标签。
  新增 `tools/dataset/evaluate_yolo_label_pairs.py`，在固定 IoU=0.50 下与人工标签比较：总体
  Precision 91.67%、Recall 42.58%、F1 58.15%、匹配框平均 IoU 98.68%。逐类 Recall：手机 3.70%、
  杯子 13.46%、键盘 50.00%、鼠标 30.77%、笔记本 86.67%、书本 57.43%。这只是同源训练候选的
  微调前诊断，不是独立 test 结果；结论是模型当前主要问题为漏检，微调应优先强化 cup/book（以及
  样本足够时的手机/鼠标），笔记本在该序列的定位/召回并不低。
- 已建立隔离候选数据集 `/home/rambos/datasets/alertgateway_4k_refine/candidate_train_20260717/`：
  同一视频前 54 张为 train、后 13 张为临时 val，全部为硬链接且不改动标注。GTX 1650 SUPER
  4GB 上使用 Ultralytics 8.4.67、YOLOv8s 初始权重、640、batch=4、AMP 关闭完成 5 epoch
  smoke 与 30 epoch 候选训练；产物为
  `/home/rambos/datasets/alertgateway_4k_refine/runs/yolov8s_4k_refine_30e/weights/best.pt`，
  最佳 epoch=25，临时 val Precision/Recall/mAP50/mAP50-95 为 90.86%/78.83%/84.45%/72.49%。
  最佳权重单独复评 val 得 83.80%/72.93%，逐类 AP50：手机 82.55%、杯子 86.50%、键盘 78.09%、
  鼠标 91.09%、笔记本 74.22%、书本 90.36%。权重加载正常，未导出 ONNX/RKNN、未上板部署。
  该 val 与 train 同源且连续，数值只作训练链路/候选趋势，不得作为精度验收或生产切换依据；下一步
  必须完成 `test_annotation/` 的另一段 63 张标注并作独立评估。
- 已建立独立测试标注工作区 `/home/rambos/datasets/alertgateway_4k_refine/test_annotation/`，只包含
  不参与训练的 `VID_20260712_131553.mp4` 的 63 张帧；图片是从 `raw/` 建立的硬链接，58 个已有
  预标注标签是从 `annotation/excluded/labels/` 复制而来，编辑不会影响原候选。剩余无预标注帧为
  `00014`、`00017`、`00032`、`00034`、`00036`。下一步由用户人工核对这 63 张并保存，之后再
  核验标签并创建正式 train/test 目录；当前尚未训练或转换。
- 用户已完成该独立测试集 63 张的人工标注：63 图/63 标签、242 个框，格式和坐标校验通过（手机 29、
  杯子 28、键盘 45、鼠标 36、笔记本 37、书本 67）。候选权重在此从未参与训练的测试集标准结果为
  Precision 82.9%、Recall 65.4%、mAP50 72.96%、mAP50-95 61.66%；AP50：手机 21.07%、杯子
  86.72%、键盘 70.06%、鼠标 98.54%、笔记本 82.56%、书本 78.81%。
- 固定 `conf=0.30`、NMS IoU=0.45、匹配 IoU=0.50 的对比中，当前 ONNX 基线 → 候选：总体 Recall
  42.98% → 65.29%，杯子 3.57% → 85.71%，书本 11.94% → 67.16%，鼠标 72.22% → 97.22%；手机
  34.48% → 17.24%、键盘 57.78% → 46.67%、笔记本 89.19% → 75.68%，总体 Precision 92.86% →
  81.03%。候选改善 cup/book 漏检但存在手机、键盘、笔记本回退，**不得导出 ONNX/RKNN 或上板部署**。
  下一步应仅从第三段、未作为该 test 的视频补充训练样本，重点补手机/键盘/笔记本的差异场景，然后
  与既有 67 张训练标签合并微调；63 张独立 test 必须保持冻结用于复验。
- 新增 `tools/dataset/export_yolo_predictions.py` 以原始图片 stem 导出微调模型预测并与 GT 配对；
  首版发现 Ultralytics 对列表输入会给 `result.path` 临时命名 `image0` 等，已修复为按输入路径命名。
  错误命名的隔离预测保留在
  `candidate_predictions_131553/invalid_export_labels/`，正确预测位于同目录 `labels/`，人工标签未改动。
- 为下一轮训练补样建立 `/home/rambos/datasets/alertgateway_4k_refine/supplement_annotation/`，来源为
  未用作独立 test 的 `VID_20260712_131214.mp4`：83 张 `raw/` 硬链接，75 个已有预标注标签复制，
  无候选帧为 `00045`--`00048`、`00076`--`00079`。该工作区只用于补充训练，不得与冻结的
  `test_annotation/` 混用；用户应优先核对手机、键盘、笔记本，保留有姿态/距离/遮挡差异的帧并删除
  连续重复帧，后续再与既有 67 张训练标注和历史桌面六类训练集混合。
- 用户已完成补充集 83 张标注核验：83 图/83 标签、1 空标签、无格式错误；实例数为手机 55、杯子 47、
  键盘 59、鼠标 61、笔记本 37、书本 0。第二轮混合训练集为历史六类 train 372 + 首轮 4K 67 + 补充
  4K 前 73 = 512 张，临时 val 为历史 val 30 + 补充末 10 = 40 张，已确认冻结的 63 张 test 没有混入。
  YOLOv8s/640/batch4/AMP关闭训练 20 epoch，候选位于
  `runs/yolov8s_4k_mixed_20e/weights/best.pt`，未导出/部署。
- 第二轮在冻结 test 的标准 mAP50/mAP50-95 为 69.83%/61.71%，AP50：手机 44.98%、杯子 63.81%、
  键盘 91.79%、鼠标 96.97%、笔记本 80.70%、书本 40.71%。固定阈值 Recall：17.24%、57.14%、
  77.78%、97.22%、70.27%、16.42%，总体 52.89%。相对首轮 4K-only 候选，手机/键盘改善而
  cup/book 回退，历史训练样本等权混入稀释 4K 特征；两轮均拒绝转换/部署。下一步应锁定 63 test，
  用独立 val 而非 test 选择 4K 重复/加权与历史样本比例，兼顾两组类别。

## 2026-07-17 4K 来源加权重训结论（当前最佳未部署）

- 原 63 张 `VID_20260712_131553` 虽从未进入训练，但已用于比较多轮候选并改变方案，现正式降级为
  4K dev，不再视为一次性 final test；部署前必须从未使用的新视频建立 final test。
- 新增 `tools/dataset/prepare_4k_weighted_refine.py`：生成历史 train 372 张一份 + 两段 4K 共
  150 张各三份的硬链接训练集，总计 822 张（4K 54.7%），组合 dev 为 4K 63 + 历史 29 共 92 张；
  训练/验证唯一图像哈希重叠为 0，来源记录在 `manifest.csv`。
- 新增 `tools/dataset/train_4k_weighted_refine.py`，支持显式学习率和冻结层。历史六类权重初始化的
  2 epoch/25 epoch 候选以及 4K 权重初始化的全量 2 epoch 候选均未同时满足目标；组合 fitness 会
  掩盖域偏移，后续必须分别评估 4K dev 与历史回归域。
- 当前 4K 最佳：从首轮 4K-only best.pt 初始化，冻结前 10 个 backbone 模块，仅训练颈/头 2 epoch，
  AdamW `lr0=0.0005`、640、batch4、AMP 关闭。权重位于
  `/home/rambos/datasets/alertgateway_4k_refine/runs/yolov8s_4k_first_head_weighted_2e_20260717/weights/best.pt`。
- 4K dev mAP50/mAP50-95 为 79.91%/70.95%，AP50 为手机 50.52、杯 85.56、键盘 86.09、鼠标
  99.39、笔记本 81.93、书 75.98。固定 conf0.30/NMS0.45/匹配 IoU0.50 的总体 P/R/F1 为
  86.21%/72.31%/78.65%，逐类 Recall 41.38/78.57/86.67/100/78.38/55.22%，匹配框平均 IoU 92.68%。
- 历史 29 张回归域 mAP50/mAP50-95 仅 65.26%/53.03%，低于历史基线 77.18%，主要是历史 mouse/
  laptop 回退。因此当前权重只允许作为 4K 浮点测试候选，不导出 ONNX/RKNN、不上板、不替换生产。
- 下一步：停止继续使用 63 张 dev 调参；建立全新 final test。若要求一个模型同时覆盖历史/V4L2 与
  4K，需先补历史 mouse/laptop 和新 4K laptop 数据，并把历史回归 mAP50 恢复到至少 72.18%。

## 2026-07-17 板端候选模型真实推流验证

- 用户明确要求查看“视频推到板子、板端推理后再传出”的结果；为此建立临时配置
  `config/config_4k_candidate_20260717.json`，模型路径为板端独立文件
  `model/yolov8s_rockchip_dfl_candidate_20260717.rknn`，未覆盖生产模型。
- 当前最佳权重已导出为九输出 Rockchip ONNX，并用 32 张 640 校准图生成 INT8 RKNN；转换日志确认
  所有层均为 INT8、无 float/fp16 fallback。该 32 张校准模型仅作本次冒烟观看，不代表最终量化验收。
- 板子 `192.168.0.200` 从 `~/AlertGateway` 启动候选配置成功：RKNN API 2.3.0、`outputs=9`、
  NPU 约 26--28 ms，持续输出 `objs:1--3`；输入为 `VID_20260712_131553.mp4` 经
  `rtmp://192.168.0.168/live/testsrc2`，输出固定为 `rtmp://192.168.0.168/live/alertgateway`。
- SRS 重启后 API 确认输入和输出均为活跃 3840x2160 H.264；本次只验证真实链路和画面观看，候选仍
  不得替换生产，也未执行 git commit。

## 2026-07-18 4K 直接照片标注工作区

- 用户采集的 4K JPG 位于 WSL 映射路径 `/mnt/g/source/vscode/AlertGateway/`，按六类目录组织：
  `cell phone` 62、`cup` 58、`keyboard` 59、`mouse` 77、`laptop` 75、`book` 77，共 408 张。
- 原始 JPEG 的编码像素均为 3840x2160，但 EXIF 方向混合；按方向旋正后得到横向 16:9 共 240 张
  （3840x2160）和纵向 9:16 共 168 张（2160x3840）。SHA-256 无完全重复文件，原始 Windows 目录
  未移动或修改。
- 已新增 `tools/dataset/prepare_4k_photo_annotation.py`，并将图片复制到独立工作区
  `/home/rambos/datasets/alertgateway_4k_photo_20260718/annotation/`。工作区包含 408 张图片、六类
  `classes.txt`、`photo_manifest.csv`，`labels/` 当前为空；原始目录提示类不等同于最终标签，标注时
  必须记录画面中所有可见目标。
- 已新增 `tools/dataset/normalize_4k_photo_images.py`，生成去 EXIF、质量 95 的预处理工作区
  `/home/rambos/datasets/alertgateway_4k_photo_20260718/preprocessed/landscape_16x9/` 和
  `preprocessed/portrait_9x16/`；主 16:9 测试应使用前者，纵向图片只作可选补充，不强行裁剪。
- 已新增 `tools/dataset/prepare_4k_letterbox_inputs.py`：它会按 `InferThread.cpp` CPU 兜底的实际
  最近邻缩放和 BGR(114) 居中补边，生成无损 640x640 PNG 模型输入预览及坐标映射清单；不替代原生
  4K 标注图。已为横向 240 张实际生成
  `/home/rambos/datasets/alertgateway_4k_photo_20260718/model_input_640_letterbox/`：每张为 640x640
  PNG，有效内容 640x360、上下各 140px 灰边，`letterbox_manifest.csv` 记录原图到模型坐标的 1/6
  缩放与 padding。
- 用户要求把预处理照片全部加入可编辑工作区。已新增
  `tools/dataset/prepare_4k_preprocessed_annotation.py`，在
  `/home/rambos/datasets/alertgateway_4k_photo_20260718/annotation_preprocessed/` 创建统一标注目录：横向
  240 + 纵向 168 = 408 张，`images/` 均为到预处理 JPG 的硬链接，未复制、移动或修改图片，`labels/`
  初始为空。
- 已启动并从 Windows 侧确认本机标注服务：`http://127.0.0.1:8765/`，当前目标为
  `annotation_preprocessed/`，API 返回 408 张图片和六类固定类别。标注仅在用户点击保存或明确标为空负
  样本后写入该目录的 `labels/`；不要标注 640x640 PNG 预览。照片用于静态检测精度评估，正式通过后仍需
  用短视频完成板端时序和推流验收。当前照片主要来自同一室内环境，暂不宣称覆盖多背景泛化。
- 标注器原先在未选中已有框时点击类别按钮只会设置“下一新框”的默认类别，保存后不会修改旧框，容易误以为
  类别改动丢失。现已调整为：单框图片点击类别会自动选中并修改该框；多框图片未选中时会明确提示先选框，
  不再静默保存。
- 用户要求先由模型识别给出草稿。新增 `tools/dataset/prelabel_missing_yolo.py`，仅给没有标签文件的图片
  写入当前 4K 浮点候选的草稿，并在写入前二次检查，避免覆盖网页中并发保存的人工标签。实际运行后统一
  工作区已有 408/408 标签文件，共 440 框（手机 63、杯子 59、键盘 82、鼠标 80、笔记本 71、书本 85）和
  63 个空标签；这些包含模型草稿与人工标签，仍必须人工复核，空标签尤其不能直接视为已确认负样本。
- 已同步增强 `tools/dataset/pseudo_label_desktop6.py` 的并发保护；其官方 ONNX 备用预标注路径随后检查到
  408 张均已有标签而全部跳过，未覆盖任何标签。
- 用户已完成 408 张预处理照片的人工复核/标注。最终只读验收：408 图片与 408 标签一一对应，无缺失或
  孤儿标签；494 个框、0 空标签、YOLO 行格式/类别/正面积/坐标边界检查均为 0 错误（贴边坐标按 1e-5
  四舍五入容差校验）。类别框数为手机 63、杯子 58、键盘 60、鼠标 80、笔记本 78、书本 155，最大最小
  比 2.67 倍；同类 IoU≥0.95 的重复框为 0。六类带框抽样及近景大键盘框目检合理，叠放书本已分别标框。
  标注验收通过。随后按文件名拍摄时间复核发现：六个原始文件夹基本各是一段单类别连续连拍（手机 62 张
  仅手机框、杯子 58 张仅杯子框、键盘 59、鼠标 77、笔记本 75、书本 77）；按单张抽约 60 张 val 会把
  相邻同姿态连拍泄漏到 train，按整段留 val 又会让 train 缺失对应主类。因此不得从这 408 张建立可信的
  六类 val；它们可全部作为训练候选。下一步须另拍至少一段独立的六类验证照片（建议每类约 10 张、每张
  改变距离/角度/遮挡），完整人工复核后冻结为 val；部署前 final test 仍需另一批未参与调参的新场景照片。
- 用户已在 Windows 源目录下新增独立验证照片 `/mnt/g/source/vscode/AlertGateway/test/`，共 82 张：手机
  13、杯子 11、键盘 10、鼠标 11、笔记本 18、书本 19。SHA-256 与先前 408 张训练来源的精确重复为 0。
  已按同一规则生成去 EXIF、质量 95 的
  `/home/rambos/datasets/alertgateway_4k_photo_20260718/validation_preprocessed/`（横向 60、纵向 22），
  并建立硬链接标注工作区 `validation_annotation/`，82 图、labels 初始为空。
- 独立验证标注服务已启动并从 Windows 侧验证为 `http://127.0.0.1:8766/`；训练照片标注服务仍为 8765，
  两者标签目录隔离。下一步用户需人工复核这 82 张验证标签；完成后先验收并冻结为 val，当前 408 张才可
  物化为 train。最终部署 test 仍需另一批未参与训练或本轮选择的新场景照片。
- 已按用户要求使用当前 4K 浮点候选权重为验证集生成草稿标签（conf=0.30、NMS=0.45、640），结果在
  `validation_annotation/labels/` 和 `pseudo_label_manifest_4k_photos.csv`：82 份标签、122 个候选框，
  类别为手机 7、杯子 11、键盘 13、鼠标 10、笔记本 22、书本 59；其中 6 张模型无检出而写为空草稿，
  不等同于负样本。用户必须在 8766 逐张复核、改错类/框、补漏框；人工完成后再做标签验收和冻结。
- 用户已完成独立验证集人工复核。只读验收结果：82 图/82 标签、103 框、0 空标签、0 格式/坐标错误，
  类别为手机 13、杯子 11、键盘 10、鼠标 12、笔记本 18、书本 39；同类 IoU≥0.95 重复框为 0，六类带框
  抽样目检合理。验证集验收通过。
- 已新增并运行 `tools/dataset/materialize_4k_photo_train_val.py`，生成冻结数据集
  `/home/rambos/datasets/alertgateway_4k_photo_20260718/train_val_frozen/`：train=408 图/494 框，val=82 图/
  103 框，图像以硬链接加入、标签复制以冻结内容，SHA-256 图像跨集合重叠为 0，`data.yaml` 已写入六类
  定义。下一步可用该 data.yaml 做新的 4K 微调；val 只用于候选选择，不得并入训练。最终部署 test 仍需
  另一批未参与训练或本轮选择的新场景照片。
- 首个新照片候选从原 4K 最佳权重继续训练，策略为冻结前 10 个 backbone 模块、AdamW、lr0=0.0005、
  batch4、640、AMP关闭；运行时限在 epoch 18/20 后终止，但 `best.pt` 已完整写入，不能记为完整 20 epoch
  训练。冻结新 val 的独立复评为 mAP50=90.47%、mAP50-95=70.58%，AP50：手机 88.13、杯子 99.50、
  键盘 94.76、鼠标 74.33、笔记本 97.80、书本 88.31。
- 同一 `best.pt` 在历史 29 张回归集仅 mAP50=48.92%、mAP50-95=36.39%，低于至少 72.18% 的通用模型
  门禁；AP50 为手机 84.91、杯子 77.71、键盘 9.20、鼠标 2.39、笔记本 21.03、书本 98.27。结论：该权重
  仅可作为 4K 照片域浮点候选，严禁 ONNX/RKNN 导出、上板或替换通用生产模型。若要一个通用模型，下一轮
  训练必须重新混合历史 train，并用当前新 val 选模型、历史回归单独门禁；若接受 4K 专用模型，仍先需另拍
  未使用 final test 后才可进入转换验证。
- 用户已决定不再追求单一通用模型，采用双模型按来源切换：现有生产模型继续服务 V4L2/旧桌面域，新的
  4K 专用权重仅服务 4K 拉流配置。两者在单一 AlertGateway 进程中按配置二选一加载，故不会在运行时同时
  占用两份 NPU 模型内存；代价是额外约一份 RKNN 文件的磁盘空间、两套模型/校准/回归测试维护，以及切换
  来源时需重启到对应配置。4K 专用候选仍必须先用全新 final test 通过浮点评估，之后才允许导出 ONNX/RKNN
  并在独立 4K 配置中板端验证，绝不覆盖 V4L2 模型。
- 用户新增的绿白键帽照片位于 `/mnt/g/source/vscode/AlertGateway/test/keyboard2/`，共 13 张 3840x2160
  JPEG；与现有 408 张训练照片和 82 张验证照片的 SHA-256 精确重复均为 0。已用
  `tools/dataset/prepare_4k_keyboard_challenge.py` 按 EXIF 方向、JPEG 质量 95 创建独立挑战集
  `/home/rambos/datasets/alertgateway_4k_photo_20260718/final_test_keyboard_challenge/`：13 张图片、空
  `labels/`、六类定义、清单和仅作评估用的 `data.yaml`。该集绝不并入 train/val；人工标注后只对已冻结的
  4K 候选执行一次键盘泛化评估，不据此继续调参。它只验证 keyboard 的绿白键帽泛化，不能替代最终六类
  final test。标注服务已从 Windows 侧启动并验证：`http://127.0.0.1:8767/`，API 返回 13 张未标注图片；
  8765 和 8766 不受影响。
- 用户已完成该挑战集标注。验收结果为 13 图/13 标签/13 个框，全部为 keyboard，图片与标签一一对应，
  YOLO 类别、字段数和归一化几何检查均为 0 错误；带框联系表抽检确认 13 张的键盘主体均被合理覆盖。使用
  冻结的 4K 候选 `yolov8s_4k_photos_head_20e_20260718/weights/best.pt` 执行有效的只读评估（640、GPU、
  batch 1、workers 0）：键盘 mAP50=71.33%、mAP50-95=48.95%、Precision=88.5%、Recall=61.5%。这证明
  绿白键帽并非完全失效，但漏检仍较多，不能仅凭此键盘单类挑战集批准导出或部署；仍需独立、未参与选择的
  六类 final test。评估脚本此前把 `ap50` 的压缩数组错误按 0 起始类别显示，已改为根据 `ap_class` 显示，
  因此本次唯一 AP50 正确归属 keyboard（类别 2）。
- 当前阶段决定：暂停任何进一步训练、微调、ONNX/RKNN 导出和板端部署。下一步仅采集并人工标注一套新的
  六类 4K final test，建议每类 10--15 张、总计 60--90 张；必须与 408 张 train、82 张 val 和已评估的
  `test/keyboard2` 均不重叠，且应改变背景、距离、角度、光线或遮挡。建议 Windows 源目录为
  `G:\source\vscode\AlertGateway\final_test\`，下设六个类别文件夹。该集不得预标注、不得参与训练或
  候选选择；人工标注验收后只对当前冻结的 4K 浮点候选做一次全六类最终评估。只有该评估达标后，才恢复
  ONNX 导出、4K INT8 校准/RKNN 转换、ONNX/RKNN 精度对比，以及使用独立 4K 配置的板端推流/稳定性验证；
  期间绝不替换现有 V4L2 生产模型。
- 2026-07-19 已核实上述 Windows 来源目录在 WSL 中映射为
  `/mnt/g/source/vscode/AlertGateway/final_test/`：共 166 张 JPG，分类目录数量为手机 29、杯子 26、
  键盘 32、鼠标 28、笔记本 25、书本 26。该批数据现指定为 4K 六类 final test 候选；后续仅可建立
  隔离标注工作区并人工标注、验收和对当前冻结候选进行一次只读最终评估，严禁预标注、并入 train/val
  或用于任何候选选择。
- 已为 final test 建立 EXIF 旋正、JPEG 质量 95 的隔离副本
  `/home/rambos/datasets/alertgateway_4k_photo_20260718/final_test_preprocessed_20260719/`（横向 150、
  纵向 16），以及仅含硬链接图片和独立 labels 的标注工作区
  `/home/rambos/datasets/alertgateway_4k_photo_20260718/final_test_annotation_20260719/`。标注服务已启动并
  验证为 `http://127.0.0.1:8768/`，返回六类和 166 张待标图片；该集绝不使用模型预标注。
- 用户已完成 final test 人工标注。只读验收：166 图/166 标签、221 个框、0 个空标签、0 个格式/类别/坐标
  错误；类别框数为手机 58、杯子 26、键盘 32、鼠标 28、笔记本 25、书本 52。与 4K train、val 和
  keyboard challenge 的源 JPEG SHA-256 精确重叠均为 0。该集已冻结，严禁再用于训练、候选选择或调参。
- 已对冻结的 4K 浮点候选
  `runs/yolov8s_4k_photos_head_20e_20260718/weights/best.pt` 执行 final test 评估（640、GPU、batch 1、
  workers 0）：Precision 89.6%、Recall 82.3%、mAP50 92.22%、mAP50-95 61.68%；AP50 为手机 81.33%、
  杯子 88.33%、键盘 94.65%、鼠标 92.74%、笔记本 98.74%、书本 97.51%。结果达到进入 4K 专用模型
  ONNX 导出、INT8 校准/RKNN 转换及 ONNX/RKNN 精度对比阶段的条件；仍绝不替换 V4L2 生产模型。
- 已从该候选导出九输出 Rockchip ONNX
  `/home/rambos/yolov8_models/onnx/yolov8s_4k_photos_head_20e_20260718_finaltest_native_640.onnx`，并仅从
  冻结训练集选择 150 张生成校准集
  `calibration_640_finaltest_20260719/`。INT8 RKNN 已输出为
  `/home/rambos/yolov8_models/rknn/yolov8s_4k_photos_head_20e_20260718_finaltest_int8.rknn`（约 12 MB）；
  转换日志确认所有层为 INT8、无 float/fp16 fallback。
- 使用同一 final test、Rockchip DFL 后处理、letterbox、conf=0.25、NMS=0.45、匹配 IoU=0.50 的量化
  对比：ONNX mAP@0.5=87.73%，RKNN Simulator INT8=87.79%（+0.06 个百分点）。逐类 AP50 ONNX→RKNN：
  手机 66.54→66.40、杯子 88.90→84.48、键盘 92.67→92.35、鼠标 84.48→85.14、笔记本 95.84→99.54、
  书本 97.97→98.85；没有不可接受的量化回退。该 4K 专用候选现允许进入隔离板端推流/稳定性/延迟验证，
  但严禁覆盖 V4L2 生产模型、配置或输出以外的正式资源。
- 2026-07-19 已将候选 RKNN 上传到 `firefly@192.168.0.201` 的独立文件
  `~/AlertGateway/model/yolov8s_4k_photos_head_20e_20260718_finaltest_int8.rknn`，SHA-256 与主机一致；
  独立配置为 `~/AlertGateway/config/config_4k_candidate_20260719.json`，使用既有 4K `testsrc2` 输入和固定
  输出 `rtmp://192.168.0.168/live/alertgateway`，未覆盖生产文件。首次启动在 StreamThread 连接输出 RTMP 时
  失败：`192.168.0.168:1935` 返回 Connection refused；MQTT 已连接，程序已读取 active 4K 配置，但未进入
  拉流/NPU 推理。主机对 SRS API `:1985` 也未获得流状态。下一步仅需恢复 SRS 的 RTMP 服务后重跑相同 120 秒
  隔离候选 smoke，再检查推流、推理耗时和稳定性；不得改用其他输出地址。
- 板端隔离部署契约已固化为仓库配置 `config/config_4k_candidate_20260719.json`：201 板使用独立候选 RKNN、
  复用 `config_4k_18mbps.json` 的 4K 输入，输出固定为 `rtmp://192.168.0.168/live/alertgateway`，不修改
  `config/config.json` 的 V4L2 生产模型入口。SRS 启动失败的根因已在 Windows 只读检查中确认：TCP 保留端口范围
  为 7998--8097，覆盖 SRS HTTP 的 8080；WSL/Linux SRS 因此在 bind(8080) 返回 EACCES。板端测试仅作为
  RTMP client 连接 1935，不会也不能造成该服务器端 bind 权限错误。SRS 应关闭不需要的 `http_server`，或改为
  保留范围外的端口后再配置开机自启。
- SRS 实际安装在 Windows `D:\Tool\SRS 5.0\SRS`。2026-07-19 已确认报错实例加载的是
  `conf\console.conf`（不是 `srs-live.bat` 使用的 `live.conf`）；此前其中 HTTP server 监听 8080，恰好落入
  Windows TCP 保留范围 7998--8097。已备份为 `console.conf.bak.20260719_1400` 并将该 HTTP server 关闭，
  保留 RTMP 1935、HTTP API 1985 和 RTC UDP 8000。手动启动后 `srs.exe` 保持运行，1935/1985 均确认监听，
  日志无 8080 bind 失败。计划任务创建因当前权限被拒绝，已改用用户 Startup 文件夹中的
  `SRS AutoStart.cmd`；脚本先切换到安装目录再运行 `objs\srs.exe -c conf\console.conf`，会在该 Windows 用户
  每次登录后自动启动。若需要无人登录即启动，应以管理员权限将同一脚本注册为 AtStartup 计划任务。
- SRS 修复后已完成 4K 专用候选的隔离板端验收：使用主机临时循环发布
  `VID_20260712_131553.mp4` 至输入 `rtmp://192.168.0.168/live/testsrc2`，201 板以
  `config_4k_candidate_20260719.json` 运行 120 秒后由 SIGTERM 正常退出（Done）。候选 RKNN 成功加载为
  9 输出/zero-copy；检测持续为 3--6 个目标，NPU 通常 26--28 ms（单次 40 ms 尖峰），总推理通常约
  36--45 ms。4K 编码/输出稳定约 30 FPS、约 18 Mbps，`put_fail=0`、`out_drop=0`、`write_fail=0`、
  `queue=0`；SRS 记录 `/live/alertgateway` 累计接收 3393 帧、约 18 Mbps。候选已通过 float、INT8、
  4K 推流和 120 秒稳定性验收，但仍仅限独立 4K 拉流配置，不覆盖 V4L2 生产模型。
- 用户指定后续板端通信统一使用 `192.168.0.200`（替代此前候选验证所用的 .201）。已核对 .200 上的候选
  RKNN SHA-256 与主机一致，且已有相同的 `config_4k_candidate_20260719.json`。当前为人工观看，正在将
  `VID_20260712_131214.mp4` 循环发布到 `/live/testsrc2`，.200 板候选输出固定为
  `rtmp://192.168.0.168/live/alertgateway`；SRS 确认结果流为活跃 H.264 Main 3840x2160。
- 用户在该人工观看中报告：整体识别稳定性有所提升，但曾出现一次将黑色桌角误检为 `laptop`。这属于
  4K 专用候选的真实场景 hard negative（背景/桌面局部）误检观察，尚未取得对应帧和置信度，不能据此调整
  阈值或重新训练。后续应从该视频定位并保存误检帧，作为只含背景的空标签 hard negative 加入下一轮训练集；
  final test 仍保持冻结，不能用于这项补强。
- 针对观看端没有时间显示、用户无法手工定位误检帧的问题，已确定后续采用非侵入式“误检留证”方案：
  临时测试配置启用 MQTT 检测缩略图，主机按消息中的 `laptop` 标签自动保存缩略图及接收时间，并与同次保存的
  原始输入/结果流片段按同一启动时间轴关联；由开发侧筛选黑桌角误检并导出全分辨率原图。该流程不依赖用户
  报时、不修改生产/V4L2 配置，也不改动已冻结 final test；确认样本后才作为空标签 hard negative 进入下一轮
  4K 训练集。
- 用户要求再次观看保留的 `VID_20260712_131214.mp4`。本次发现 SRS 进程存在但 1935/1985 未监听，已按已修复
  `console.conf` 重启 SRS；随后主机重新发布到 `rtmp://192.168.0.168/live/testsrc2`，`.200` 候选配置成功
  拉流并推回固定结果地址 `rtmp://192.168.0.168/live/alertgateway`。SRS API 已确认输入/输出均为活跃
  3840×2160 H.264，板端日志显示 NPU 约 26--30 ms、单帧总推理约 36--44 ms；本次观看测试最多保留 600 秒。
- 已根据本文件中已验证的 4K 数据、量化、SRS 修复和 `.200` 板端验收事实，新增知乎风格技术复盘
  `docs/4k/4k_specialized_model_final_acceptance_20260719.md`。文章按“发现问题→分析原因→设计方案→解释原理
  →实施验证→展示结果→复盘不足”组织，明确区分 train/val/challenge/final test，且不把不同数据集的分数
  伪装成直接前后对比。
- 2026-07-19 用户在 4K 专用模型实流观看中确认：识别/画框准确率提高，但目标移动时卡顿感比此前更明显。核对仓库与 192.168.0.200 板端实际 `config_4k_candidate_20260719.json` 后确认，新候选通过 `active_config=config_4k_18mbps.json` 继承基础链路，仍使用代码默认的自适应跟踪参数，但没有继承上次人工观看候选的 `track_center_gated_size_filter=true` 与 `track_global_motion_center_filter=true`，因此此前卡顿/抖动优化只保留了基础部分，并非完整保留。板端日志同时显示输出约 30 FPS、推理总耗时约 38～44 ms，未见编码或推流掉帧证据；当前更可能是约 24～26 FPS 的检测更新节拍叠加框平滑策略回退造成的框运动阶梯感。下一步应只在专用模型候选配置中补回已验收的跟踪参数，保持模型、阈值和固定输出地址不变，再用同一视频做人工 A/B。
- 已将上一版 4K 实流观看候选的完整 30 项 `track_*` 参数适配进新专用模型配置 `config/config_4k_candidate_20260719.json`，逐字段比较确认缺失 0、变化 0；其中低速尺寸门控与全局运动中心滤波均显式启用。新 RKNN 路径、`conf_threshold=0.25`、六类范围、18 Mbps 基础配置和固定输出地址保持不变。配置已同步到 192.168.0.200，使用同一 `VID_20260712_131214.mp4` 启动 600 秒观看验证；SRS 已确认 `/live/testsrc2` 与 `/live/alertgateway` 均为 active 3840×2160 H.264，板端 NPU 约 25.5～29.3 ms、总推理约 35.7～45.1 ms。客观链路正常，最终移动流畅度仍需用户观看确认。
- 用户随后报告推理结果流播放持续严重卡顿。诊断确认板端 18 Mbps 运行时仍为约 29.96 FPS，编码/输出无丢帧、失败或队列积压，但 SRS 一度仅能向播放器发送约 8.5 Mbps，低于约 17.6～18 Mbps 的结果流生成速率，卡顿属于播放传输吞吐不足，不是跟踪计算造成。入口配置中的 `stream.bitrate_kbps=8000` 首次尝试因程序合并语义（`active_config` 选中配置覆盖入口配置）未生效，已纠正为新增隔离基础配置 `config/config_4k_8mbps.json`，并让专用候选选择该配置；共享的 18 Mbps 配置、生产配置和代码均未修改。192.168.0.200 实测 MPP 目标 8 Mbps，稳定窗口输入约 30.07 FPS、编码/输出约 29.96 FPS、实际约 7.93 Mbps，`enc_drop=0`、`out_drop=0`、`write_fail=0`、队列为 0。新模型和完整 30 项跟踪参数继续保留，待用户重新观看确认播放与画框运动效果。
- 已修改技术复盘 `docs/4k/4k_specialized_model_final_acceptance_20260719.md` 的结语，移除突兀的“真正困难的是克制”和连续三个“承认”，改为从训练数据、独立测试、ONNX/RKNN 一致性及板端完整推流四个环节说明识别正确率优化如何形成可验收闭环，结语重新聚焦本轮 YOLO 4K 识别正确率优化目的。
- 经 Git 提交内容审查，已从工作树删除 `b00d067` 中 18 个不应长期保留的实验产物：14 个 `motion_*_20260716.json/.md` 重复扫描输出，以及 4 个中间诊断、过渡观看或已否决方案配置。源码、分析工具、单元测试、生产配置、最终汇总文档和仍有用途的 size20 配置均保留；汇总文档中两处对已删产物的引用已改为直接说明关键结果集中保留。当前仅形成可审查的 Git 删除修改，未提交、未推送、未改写历史。
- 用户确认 `docs/4k_log*` 临时诊断文件没有必要上传。已删除 8 个文本日志及 `docs/4k_log_20260716004_frames/` 下 7 张过程抓帧（约 4.5 MB 二进制过程产物），并在 `.gitignore` 增加 `docs/4k_log_*`，防止同类日志和截图再次进入 Git；正式 4K 结论已保留在汇总报告中。
- 用户要求继续清理 `docs/4k` 后，已删除 9 个历史/重复文档：闪烁方案长篇实验记录、18 Mbps 单次推流记录、3 个一次性探针记录，以及早期拉流复现、帧率修复、6 Mbps 验收和码率矩阵记录。保留 4K 架构方案、准确率微调与 INT8 校准规范、检测框运动指标和最新 30 FPS 码率验证；`docs/4k/README.md` 已收敛为当前文档索引，30 FPS 报告已移除对已删码率矩阵的引用。所有删除和索引修改仍未提交，待用户最后统一 commit。
- 已审阅 `docs/artifacts`：目录内结构分析、候选 YAML 和剪枝/蒸馏方案共约 16 KB，且 `tools/dataset/distill_desktop6.py` 默认直接引用 `yolov8s_pruned10_model.yaml`，因此保留而不删除。新增 `docs/artifacts/README.txt`，说明各文件用途、候选结构与生产模型的边界，并明确这些文件不是板端权重。
- 按文档清理计划，已删除 `docs/架构图.html`（旧版架构图）、`docs/performance_dashboard.html`（未引用的旧性能大屏）和 `docs/3588学习计划.txt`（早期个人学习计划）。NV12/H264 图解页脚已改为引用当前 `docs/architecture/alertgateway_architecture.html`；其他历史基线、性能记录和阶段性开发文档暂时保留。
- 已同步修改根目录 `readme.md`：补充 4K final test 与 ONNX/RKNN 精度结果，增加 8 Mbps 观看配置和 4K 专用候选配置说明，将板端部署示例统一改为 `192.168.0.200`，移除不存在的知乎文章条目、旧 `v4l2_720p` 输出描述及重复的英文历史码率段落。README 仍未提交，待下一次文档提交统一纳入。
- 2026-07-19 使用 `VID_20260712_131410.mp4` 复播验证 4K 专用候选：先确认 SRS 的
  `/live/testsrc2` 输入活跃后，`.200` 板以 `config_4k_candidate_20260719.json` 拉流；SRS 已确认固定结果流
  `rtmp://192.168.0.168/live/alertgateway` 为 active、H.264 Main、3840×2160，约 6.27 Mbps。板端连续检出
  1 个目标，NPU 通常约 25.5--28.9 ms、总推理约 36.0--46.0 ms。本次采用 8 Mbps 输出和完整跟踪平滑参数，供用户人工观看误检与流畅度；未改动源码、生产配置或冻结 final test。
- 新增主机端脚本 `tools/test/run_4k_candidate_on_board.sh`，用于一键执行“视频发布到 SRS → 等待输入流
  active → `.200` 板候选拉流推理 → 固定结果流回推”。脚本不改生产配置、不杀已有 AlertGateway；会检查视频、
  SRS API、板端二进制/配置/RKNN、输入和结果流状态，并在失败时输出发布端与板端日志末尾，同时保留 `/tmp` 下的
  完整调试文件。已通过 `bash -n` 与 `git diff --check`，尚未进行该脚本自身的完整实流验收。
- 根目录 `run.sh` 已固定封装本轮人工观看命令：
  `tools/test/run_4k_candidate_on_board.sh runs/input_videos/4k/VID_20260712_131214.mp4 180`；已设为可执行，
  仅调用隔离的 4K 候选验证脚本，不改生产配置。
- 已新增 `tools/readme.txt` 与 `tools/test/readme.txt`，分别说明工具分类、当前推荐的 4K 候选入口、
  固定输出地址、板端前置条件、日志位置以及历史对比脚本的使用边界；根目录 `run.sh` 现会先切换到自身
  所在目录，因而可从任意当前目录调用。
- 根目录 `readme.md` 已补充“一键 4K 候选观看”说明：明确 `run.sh` 在主机侧将
  `VID_20260712_131214.mp4` 于 180 秒窗口内循环发布，随后以隔离候选配置运行 `.200` 板并回推固定结果流；
  同时明确板端 `start.sh` 是常驻 `config/config.json` 启动/停止与锁频管理脚本，不发布测试视频、不调用候选
  配置，也不会自动超时退出。
- 用户授权后已清理主机 `/tmp` 下的历史 AlertGateway 临时产物：删除所有旧 `/tmp/ag_*` 文件、
  `/tmp/pull_reconnect_push.log` 以及较早的候选脚本日志目录
  `/tmp/alertgateway_4k_20260719_172655/`。清理前确认没有相关 AlertGateway、FFmpeg 或一键测试进程在运行；
  保留最新 `/tmp/alertgateway_4k_20260719_173658/`（44 KB）以便继续排查，另保留两个 0 字节训练日志。
- 用户当前处于学习阶段，已确定后续开发优先级为“先单路性能，再多路并发”，并新增
  `docs/4k/4K单路性能与多路并发学习计划.md`。近期只需以 `./run.sh` 建立可重复的
  4K 单路基线，针对一个明确瓶颈做单变量验证；单路稳定且至少完成一次有结论的小优化后，再以两路 1080p
  或一路 4K 加一路 1080p 做并发 PoC。多路阶段目标是识别首个资源瓶颈和隔离需求，不承诺并发路数，也不
  改动 V4L2 生产模型或现有固定结果流规则。
- 上述学习路线已扩展为详细性能执行计划：先以 `./run.sh` 固定视频运行 180 秒（前 30 秒预热、后 150 秒统计），
  记录 Pull/Infer/Encode/Stream、SRS 与播放器证据；按“链路、观看、画框观感”分流定位，再一次只验证一个
  性能假设。进入两路 PoC 前，要求至少两次单路有效运行均约 30 FPS、无持续队列增长且
  `enc_drop/out_drop/write_fail=0`，并已完成一次有保留或回退结论的单变量实验。当前仍应先完成 8 Mbps
  单路基线，不应将历史 18 Mbps 的播放器吞吐问题误改为 NPU/模型问题。
- 2026-07-19 已按该计划完成两次 180 秒、同视频同配置的 8 Mbps 单路基线。两次旧二进制均复现拉流侧
  `enc_drop`（分别 31 与 30 帧），但编码/推流始终约 30 FPS、约 8 Mbps，`out_drop=0`、`write_fail=0`、
  队列不持续累积；因此瓶颈是 `PullStreamThread` 到容量为 1 的 `enc_queue` 的瞬时入队策略，不是 NPU 或
  MPP 吞吐。代码审查确认注释声称“阻塞投递”，实际却使用 `push(..., 0)` 的非阻塞语义。已仅将
  `src/capture/PullStreamThread.cpp` 改为与 V4L2 路径一致的 `push(..., 100)` 有界等待，并交叉编译、同步至
  `.200` 后以同一 180 秒口径回归：全程 `enc_drop=0`、`out_drop=0`、`write_fail=0`，末尾窗口输入/编码/输出
  约 30 FPS、约 7.93--8.12 Mbps、编码约 18.2--18.6 ms，NPU 约 26.7--27.3 ms、总推理约 38.8--42.3 ms。
  该单变量优化通过，未改模型、候选配置、V4L2 生产入口或固定结果流。下一步应由用户在同一 8 Mbps 结果流
  人工确认播放是否连续；若画面连续但框仍有阶梯感，再只研究检测刷新率与显示平滑策略。
- 为核实“18 Mbps 是否不能用”，已使用同一视频、同一候选模型和修复后二进制，在 `.200` 板以一次性
  `/tmp/alertgateway_4k_candidate_18mbps_ab.json` 入口配置完成 180 秒 18 Mbps A/B；该临时入口只将
  `active_config` 指向既有 18 Mbps 基础配置，结果流仍为固定地址。板端末尾窗口输入/编码/输出约 30 FPS、
  约 18.0--18.2 Mbps，`enc_drop=0`、`out_drop=0`、`write_fail=0`、队列为 0，编码约 18.6--18.9 ms，
  证明 18 Mbps 并非板端 NPU/编码/推流瓶颈。本次 SRS API 未记录独立播放器的持续接收会话，不能用其
  `send_kbps` 取代播放器实测；因此结论仅为“板端可稳定生成 18 Mbps”，18 Mbps 在当前观看端是否卡仍需
  用户用同一播放器观看时同步采集 SRS player 会话与人工现象后确认。一次性板端配置应在测试后删除。
- 随后已完成一次带真实播放器连接的 18 Mbps、180 秒观看验证：SRS 的 `alertgateway` 流持续显示
  `clients=2`（板端发布端与播放端），两次 30 秒采样分别为接收/发送约 `18.06/18.08 Mbps` 与
  `18.11/18.09 Mbps`，表明当前 SRS 到该播放器能持续送达完整 18 Mbps，不再复现此前约 8.5 Mbps 的
  发送受限。板端仍保持约 30 FPS、18 Mbps 且无丢帧/写失败。结论更新为：18 Mbps 当前可作为观看候选，
  之前卡顿是当时的观看链路瞬态或播放器状态，不能再把 8 Mbps 当作唯一可用档位；是否默认使用 18 Mbps
  仍以用户本次实际观感为准。一次性板端入口配置在结束后删除。
- 为使后续 8/18 Mbps 观看 A/B 自动留下播放器证据，已增强
  `tools/test/run_4k_candidate_on_board.sh`：结果流 active 后每 10 秒把完整 SRS streams API 快照写入
  本次 `/tmp/alertgateway_4k_时间戳/srs_watch.jsonl`，退出或 Ctrl+C 时自动停止采样。后续只需核对
  `alertgateway` 的 `clients` 与 `kbps.recv_30s/send_30s`，即可区分板端生成速率和播放器实际接收速率；
  该工具改动不改 SRS、播放器、候选配置或 V4L2 生产入口。
- 用户已完成 18 Mbps 框运动验收并确认“画面不卡顿、移动框不算滞后”。该轮客观日志同时为约 30 FPS、
  约 18 Mbps、`enc_drop/out_drop/write_fail=0`，SRS 自动快照 18 份且播放器持续接收约 18 Mbps。至此单路
  4K 的拉流、推理、编码、推流与观看验收通过；当前环境可使用 18 Mbps。为避免未经选择地改变带宽策略，
  隔离候选配置仍保持 8 Mbps，18 Mbps 作为已验证观看候选；后续若用户优先画质可显式切换候选基础配置，
  若优先节省带宽则保持 8 Mbps。单路性能下一步不再继续调框平滑，而是可在需要时进入两路 1080p 并发 PoC。
- 用户选择画质优先的推荐配置，且明确暂不进行双路并发。已将隔离候选入口
  `config/config_4k_candidate_20260719.json` 的 `active_config` 从 `config_4k_8mbps.json` 切回
  已验收的 `config_4k_18mbps.json`；8 Mbps 基础配置保留为网络受限时的手动降级选项。该变更仅影响 4K
  专用候选，不改 `config/config.json`、V4L2 生产模型、模型参数或固定结果流；下一步只需同步候选配置至
  `.200`，不启动双路测试。
- 2026-07-20 用户要求先进入双路验证，已将根目录 `run.sh` 改为调用新增的
  `tools/test/run_dual_candidate_on_board.sh`。该脚本默认启动两路 1920×1080@30 FPS：主机将两段视频分别
  发布到 `live/dual_a` 与 `live/dual_b`，板端启动两个独立的临时完整配置；A 路结果使用固定地址
  `rtmp://192.168.0.168/live/alertgateway`，B 路写入板端 `/tmp` 临时 FLV，不创建第二个结果 RTMP 地址。
  两路使用独立 MQTT topic、配置、日志和输入快照，默认每路目标 8 Mbps、运行 180 秒。当前已通过
  `bash -n` 和 `git diff --check`，尚未进行板端实流验收；生产 `config/config.json`、源码和固定 RTMP
  规则未改变。
- 随后用户明确要求统一入口兼容单路、双路和后续多路，且不增加命令行参数。已将 `run.sh` 改为注释切换
  结构：当前仅双路 `exec` 生效，单路 `run_4k_candidate_on_board.sh` 和未来多路脚本作为注释候选，
  切换时只允许保留一个未注释的 `exec`。`start.sh` 和底层单路脚本仍保持可用。
- 用户要求将代码层面的单路/双路/多路兼容方案整理成开发计划，已新增
  `docs/4k/单路双路多路兼容开发计划.md` 并加入 4K 文档索引。计划确定先保留旧平面配置兼容，再引入
  `channels[]`、路级 `ChannelPipeline`、独立队列/跟踪/资源/日志与明确输出 sink；先做双路 1080p，再做
  4K+1080p，最后按资源证据扩展多路。当前仍未改动 C++ 实现。
- 已按计划完成首版 C++ 多通道实现并通过全量交叉编译：`main.cpp` 将旧平面配置归一化为 `single` 或解析
  `channels[]`，新增 `src/app/ChannelPipeline` 让每路拥有独立 source、队列、RKNN、MPP、输出、MQTT 与
  `SharedDetections`。输出新增 `fixed_rtmp`、`local_flv`、`metrics_only` sink，进程内最多一路可使用固定
  RTMP 地址；Pull/Infer/Track/Encode/Stream 统计与 MQTT 消息携带 `channel_id`。新增
  `config/config_multi_1080p_candidate.json` 和 `tools/test/run_multi_candidate_on_board.sh`，根目录 `run.sh`
  当前默认调用该单进程双路入口。当时尚未向 `.200` 部署或进行双路实流验收；共享 NPU 调度仍未实现，
  且未改变 V4L2 生产入口、模型、阈值或固定输出地址。
- 2026-07-20 已将最新多通道二进制部署至 `.200`，使用 `VID_20260712_131214.mp4` 和
  `VID_20260712_131410.mp4` 分别发布到 `rtmp://192.168.0.168/live/dual_a` 与
  `rtmp://192.168.0.168/live/dual_b`，完成一次 180 秒单进程双路 1080p PoC。A 路固定结果流
  `rtmp://192.168.0.168/live/alertgateway` 在运行中经 SRS 确认为 H.264 Main 1920×1080；B 路
  本地 FLV 为 160,301,166 字节、159.867 秒、1920×1080@30 FPS。两路末尾
  `enc_drop=0`、`out_drop=0`、`write_fail=0`，队列无持续积压；有效推理样本显示 A/B 总耗时均值约
  41.56/42.04 ms、P90 约 54.74/54.87 ms，说明首个可见压力是双路 NPU 推理节拍，MPP/RTMP
  未饱和。`infer_drop` 是容量为 1 的最新帧推理队列覆盖计数，符合当前低延迟设计，不能与编码/输出丢帧混淆。
  两路 MQTT 已分别连接且配置为 `desk/dual/a`、`desk/dual/b`，本轮没有订阅 broker 保存 payload，故
  `channel_id` 的消息级留证仍待补充。测试结束后两个 pipeline 正常停止，远端临时配置已清理，未改生产
  V4L2 入口、模型、阈值或固定 RTMP 规则。
- 本次首轮启动暴露并修复双路测试脚本的 SRS API 兼容问题：当前 SRS 将活动状态放在
  `publish.active` 而非顶层 `active`；`run_multi_candidate_on_board.sh` 与隔离对比脚本均兼容两种
  字段。当 HTTP API 不可达时，脚本改用 `ffprobe` 直接探测 RTMP 流，避免监控 API 故障误判成流故障。
  下一步先补 MQTT payload 留证，再决定 4K+1080p 或三路扩容；暂不引入共享 NPU 调度。
- 针对“是否只是源视频”的疑问，已从板端 B 路 FLV 在 60 秒位置抽帧核对：画面明确包含绿色检测框
  与中文标签（杯子、书、鼠标），确认两路 PoC 实际执行了 RKNN 推理和编码叠框。`live/dual_a`、
  `live/dual_b` 是原始输入地址，观看时不会有框；带框的网络结果仅为固定
  `rtmp://192.168.0.168/live/alertgateway`，B 路结果仅保留在板端本地 FLV。
- 2026-07-20 用户将结果推流规则更新为按通道独立的固定模板：
  `rtmp://192.168.0.168/live/alertgateway_channel_{channel_id}`。旧平面单路配置继续使用
  `alertgateway`；当前双路 PoC 使用通道 ID `1`、`2`，对应结果地址为
  `alertgateway_channel_1`、`alertgateway_channel_2`。实现已移除“一进程最多一路 RTMP”的限制，改为校验每条
  `fixed_rtmp` 输出严格等于通道模板地址且不重复；双路配置与启动脚本已同步，待重新交叉编译和板端双结果流实测。
- 已完成规则修改后的 .200 板双路 1080p 180 秒实测：SRS 同时确认
  `alertgateway_channel_1` 与 `alertgateway_channel_2` 均为 active 的 H.264 Main 1920×1080
  推理结果流。末尾每路输出约 26.2--28.4 FPS、约 7.0--7.6 Mbps，`enc_drop=0`、
  `out_drop=0`、`write_fail=0`；两路均正常结束。首次部署失败源于 `ChannelPipeline.cpp` 遗留旧单地址
  校验，已改为与主入口一致的“单路 `alertgateway`、多路 `alertgateway_channel_{channel_id}`”规则并复测通过。
- 2026-07-20 已完成第一项推理内存优化：`InferThread` 的普通推理和 ROI Tiling 不再先写临时
  `input_buf` 再 `memcpy` 到 RKNN 输入 DMA 内存，而是让 RGA/CPU 回退路径直接写入
  `input_mem_->virt_addr`，并在 `rknn_run` 前调用 `rknn_mem_sync(..., RKNN_MEMORY_SYNC_TO_DEVICE)`。
  主机交叉编译、`git diff --check` 和脚本语法检查通过。随后重新部署到 `.200` 完成双路实流验证，
  两路结果地址 `alertgateway_channel_1/2` 均正常 active；远端日志未出现 RGA、`rknn_mem_sync` 或
  `rknn_run` 错误，`cpy` 阶段主要为约 0.1--0.5 ms 的 cache sync，不再是约 1.2 MB/帧的显式 memcpy。
  当前仍保留采集帧副本、FFmpeg 解码到 NV12 的复制和 MPP 编码输入复制；下一步如继续优化，应先做
  引用计数帧/缓冲池设计并单独验证生命周期，暂不直接改硬件解码链路。
- 2026-07-20 已完成第二项帧复制优化：新增 `SharedByteBuffer`，`Frame::raw_data` 的底层
  `std::vector<uint8_t>` 改为 `shared_ptr` 共享存储。V4L2 的 `mmap -> Frame` 仍保留一次必要复制，
  但同一帧进入编码队列和推理队列时只复制 Frame 元数据/引用计数，不再复制整帧像素；FFmpeg 拉流
  路径同样共享编码队列与推理队列的 NV12 存储。ROI Tiling 仍显式复制 tile，这是其独立推理输入所必需。
  已兼容现有 `resize/data/size/empty/operator[]` 使用方式，交叉编译和 `git diff --check` 通过。
  新二进制已部署到 `.200` 并完成双路实流验证：两路持续输出推理日志，末尾示例 channel 1 为约
  29.7 FPS、约 7.87 Mbps，`enc_drop=0`、`out_drop=0`、`write_fail=0`、队列为 0；`cpy` 通常约
  0.02--0.08 ms，未出现 RGA、RKNN 或帧生命周期错误。下一步可在需要时单独研究 V4L2/MPP DMA
  buffer 直通；当前不继续扩大改动范围。
- 2026-07-20 对 V4L2/MPP DMA 直通进行了实现前评估：当前 CMake 和源码未接入 FFmpeg RKMPP
  硬件解码或 AVFrame DMA buffer，`PullStreamThread` 使用 FFmpeg 软件解码后转/拷贝为 NV12；V4L2
  入口使用 YUYV `mmap`，而 `EncodeThread` 的 MPP 输入需要 NV12 DRM buffer。由于像素格式、内存
  类型和缓冲区归还生命周期均不匹配，当前不直接实现跨模块 DMA 直通，避免破坏单路/多路稳定性。
  后续若要继续，应先单独引入并验证 RKMPP 解码输出、DMABUF fd 传递、RGA/MPP 同步和异常回收，
  形成独立实验分支后再合入主流程。
- 2026-07-20 进一步在 `.200` 板使用真实 RTMP 输入短时探测 `h264_v4l2m2m`：FFmpeg 能识别该
  解码器，但打开阶段报 `Could not find a valid device` / `No such device`，未能建立硬件解码会话。
  因此当前不能把 V4L2 M2M 自动接入 `PullStreamThread`，否则会使现有拉流启动失败；继续保持
  FFmpeg 软件解码路径。后续硬件解码需要先解决板端 V4L2 M2M 设备注册/驱动或引入可用的 RKMPP
  FFmpeg 构建，再进行独立实验。
- 2026-07-20 已实现第三项内存优化：新增 `FrameBufferPool`，由每个 `ChannelPipeline` 独立持有，
  V4L2 和 FFmpeg 拉流生产路径复用已释放的同尺寸 `std::vector<uint8_t>`；池不阻塞采集，缓存不足时
  自动分配，队列/消费者释放最后一个共享引用后才归还。保留旧的三参数 `CaptureThread` 构造兼容性，
  未传池时自动创建本地池。已修正池回收策略：保留 vector size/capacity，同尺寸帧直接覆盖写入，避免
  每帧 clear/resize 造成额外初始化。交叉编译和 `git diff --check` 通过；部署到 `.200` 后完成双路
  实流验证，两路持续输出推理/编码/推流统计，未出现 RKNN/RGA 错误，正常窗口 `enc_drop=0`、
  `out_drop=0`、`write_fail=0` 且队列无持续积压。当前池只复用 CPU 像素 vector，不改变 V4L2 到
  Frame 的必要复制，也不改变 ROI Tiling 的独立 tile 复制。
- 2026-07-20 已完成 ROI Tiling 的低复制路径改造：`InferThread::infer_once` 支持传入 `TileRect`，
  RGA 使用原始帧的 `srect` 直接读取 tile 区域，并将结果写入 RKNN 输入 DMA 内存；只有 RGA 失败时
  才调用原有 `TilingTask::crop()` 复制 tile 后回退。普通推理路径保持原行为，tile 检测结果仍按
  tile 坐标偏移回整帧坐标。交叉编译和 `git diff --check` 通过；当前双路候选配置 ROI/Tiling 为关闭状态，
  因此本次尚未完成启用 Tiling 的板端实流验收，后续需用专门开启 ROI/Tiling 的候选配置验证 NV12/YUYV
  stride、边界和检测框坐标。
- 2026-07-20 已新增单路 ROI 候选配置 `config/config_4k_roi_candidate_20260720.json`，并将根目录
  `run.sh` 临时切换为“单路 4K + 全帧 ROI + 2 列 Tiling”入口，先于双路验证 ROI 逻辑。期间发现旧
  `active_config` 入口不是递归继承，且 `config_4k_18mbps.json` 不含 model 字段；已让 ROI 候选显式
  提供 4K source 和固定 `alertgateway` output，并继承完整候选模型配置，同时增强单路测试脚本的
  SRS active 字段兼容和候选配置部署。
- 已完成单路 ROI/Tiling 120 秒板端实流验证，日志目录为
  `/tmp/alertgateway_4k_20260720_194119`。板端确认 `Running ROI Tiling with 2 tiles` 持续执行，
  没有 RGA/RKNN 错误；正常窗口约 30 FPS、约 18 Mbps，`enc_drop=0`、`out_drop=0`、`write_fail=0`、
  队列无持续积压，`active_tracks` 持续有检测目标。说明 RGA 直接读取 tile 区域、两 tile 推理、
  坐标还原和整帧编码推流链路已跑通。当前 `run.sh` 保持单路 ROI 默认，待确认后再切回双路多通道。
- 用户提出大目标跨 tile 边界可能产生左右两个检测框，已在
  `docs/4k/单路双路多路兼容开发计划.md` 新增边界目标处理计划：先比较 20%/30% tile overlap，
  再实现相邻 tile、同类别、靠近共同边界框的安全联合与全局 NMS，最后才评估整帧低频补检。当前
  尚未修改默认检测合并规则，先按单路基线→重叠率→边界联合→双路的顺序验证，避免增加 NPU 负载后
  无法区分收益来源。
- 随后已实现第一版跨 tile 边界框联合：`TiledDetection` 保留 tile 归属，左右/上下相邻 tile 中
  同类别、贴近共同边缘、正交重叠至少 45%、边界间距不超过 96 像素的框先做 union，再做全局 NMS；
  日志输出 `ROI boundary merges=N`。已部署并完成单路 4K + 2 tile 短时实流验证，日志多次出现
  `ROI boundary merges=1`，末尾约 30 FPS，`enc_drop=0`、`out_drop=0`、`write_fail=0`。当前只证明
  合并路径实际执行且推流稳定，尚未完成相邻真实目标的误合并率测试；下一步应补充边界样本对比，
  再决定是否提高 overlap_ratio 到 20%/30%。
- 用户继续发现整帧复检与 tile 结果可能出现“大框套小框”。已在 `TilingTask::merge()` 增加同类别
  containment NMS：交集/较小框面积达到 `0.70` 且小框中心在大框内时抑制重复框，分数接近时保留
  更大框，不同类别不抑制。单路 ROI 实流复测已通过：整帧复检、边界合并均持续执行，正常窗口约
  30 FPS，`enc_drop=0`、`out_drop=0`、`write_fail=0`。当前仍需用两个相邻同类真实目标样本做
  误合并验收，不能仅凭单一视频确认所有场景。
- 2026-07-20 已完成 containment NMS 版本的单路 4K + ROI/Tiling 180 秒播放测试，日志目录为
  `/tmp/alertgateway_4k_20260720_203815`。测试期间结果流为固定
  `rtmp://192.168.0.168/live/alertgateway`，整帧复检和 `ROI boundary merges=1` 持续执行；末尾
  统计约 30 FPS、约 17.8--18.2 Mbps，`enc_drop=0`、`out_drop=0`、`write_fail=0`、队列无持续积压。
  测试结束后输入发布和板端进程已清理；播放地址当前不再 active，下一次观看需重新启动测试入口。
- 用户实测仍反馈手机、笔记本偶发重叠。复核后确认编码器叠框读取的是 `SharedDetections` 的跟踪快照，
  重复检测可能在前一帧分别建立两个 track，后续即使原始 tile 框被抑制，旧 track 仍会在 `display_hold_ms`
  窗口内同时显示。已在 `SharedDetections::publish_snapshot()` 增加最终显示层同类别去重：按 IoU、包含关系，
  以及正交重叠至少 45% 且间隔不超过 96 像素的近邻框合并/抑制；不同类别不处理。去重只影响编码显示快照，
  不改变跟踪器关联状态。交叉编译通过，并完成板端 45 秒实流验证，日志目录为
  `/tmp/alertgateway_4k_20260720_205323`；ROI 边界合并和整帧重复抑制均持续执行，推流链路无编码/推流错误。
  仍需用户在手机、笔记本实际重叠片段上观看复核；若仍有重叠，应抓取该帧的显示框坐标，进一步区分是两个
  同类 track、不同类别框，还是播放器/旧流缓存导致。
- 针对用户反馈的“手机/笔记本偶发重叠框”，已继续增加 tile 结果与整帧复检结果之间的跨来源抑制：
  `TilingTask::suppress_tile_duplicates()` 仅处理同类别，要求 tile 框与整帧框交集占 tile 框面积至少
  `0.20` 且中心距离不超过较小框尺度的 `0.55`，命中时删除 tile 框、保留整帧框；不同类别不抑制。
  这样整帧框作为完整目标结果优先，避免手机/笔记本被 tile 边缘结果重复绘制。已重新交叉编译并完成
  单路 4K + ROI/Tiling 45 秒板端实流验证，日志目录为 `/tmp/alertgateway_4k_20260720_204739`，
  多次出现 `full-frame suppressed tile duplicates=1/2`，同时 `ROI boundary merges=1`，说明新抑制路径
  实际生效且推流链路稳定；仍需用手机、笔记本相邻/跨边界的真实片段做画面级复核，确认不会误删两个独立同类目标。
- 根据用户进一步分析，已将处理重点前移到 Tracker 之前：`TilingTask::make_tiles()` 不再使用旧的比例
  放大后再 clamp 的方式，而是按 ROI 尺寸计算 384--512 像素（当前 4K ROI 约 448 像素）的明确左右/上下
  tile overlap；`merge_boundary_detections()` 增加 IoS（交集/较小框面积）至少 `0.35` 的重叠区去重，
  两个框清晰度差异明显时优先保留离 tile 边缘更远的框，接近时 union。边界复检由原来的 full-frame
  `infer_once(frame, nullptr)` 改为当前 ROI 的 `infer_once(frame, &roi_rect)`，坐标还原后与 tile 结果
  融合，再统一 `merge()`，最后才调用 `shared_dets_.update()` 进入 Tracker。已移除此前增加的最终显示层
  去重，避免继续依赖 Tracker 后补救。交叉编译和 `git diff --check` 通过；下一步必须重新部署并用播放器
  观察手机/笔记本片段，且要确认测试脚本确实保存了非空板端日志。
- 2026-07-20 针对仍偶发的手机/笔记本重框，已完成第二版 Tracker 前融合：不再根据 tile 物理外边缘决定
  是否复检，而是仅对两个相邻 tile 的公共重叠带内、同类别、正交方向重合的候选对生成联合 ROI；联合 ROI
  覆盖两个半框及周边上下文，复检后的完整框在 `SharedDetections::update()` 前替换覆盖它的 tile 半框。跨来源
  抑制取消了会误拒绝“半框 + 完整框”的中心距离限制，改为 tile/full 覆盖率至少 50% 或 IoS 至少 35%。
  `overlap_ratio` 重新参与 384--512 像素 overlap 计算，便于后续单变量调整。新增 image_processing_smoke 的
  4K 几何与替换回归断言；交叉 `cmake --build build -j4` 与 `git diff --check` 通过。尚未部署到板端，
  不能宣称手机/笔记本视觉问题已解决；下一步须部署后观察日志 `ROI overlap candidates`、
  `joint-ROI suppressed tile duplicates` 并人工观看跨缝片段。
- 2026-07-20 已部署第二版融合并完成两次板端实流。首轮 120 秒逻辑已实际触发 636 次联合 ROI、
  634 次 tile 重复框抑制，链路正常结束；同时发现动态联合 ROI 仅偶数对齐，RK3588 RGA 的 NV12
  裁剪要求宽度/起点 16 对齐，导致复检大量回退 CPU。已将联合 ROI 横向向外对齐到 16 像素，并令
  letterbox 目标矩形保持偶数尺寸/起点，重新交叉编译和部署。后续 45 秒复测日志
  `/tmp/alertgateway_4k_20260720_215021/board.log`：联合 ROI 319 次、tile 重复抑制 319 次、
  `RGA letterbox failed=0`，稳定窗口约 30 FPS、18 Mbps，`enc_drop=0`、`out_drop=0`、
  `write_fail=0`，正常 `Done`。这验证了新融合和 RGA 路径，不构成手机/笔记本画面级最终验收；
  下一步应由用户观看固定结果流中的跨缝片段确认是否仍有重框。
- 2026-07-20 根据用户明确的 Tracker 输入约束，已将 `merge_boundary_detections()` 改为全局分割线融合：
  所有 tile 框先映射回整帧坐标；相邻 tile 的同类框仅在靠近公共分割线、正交方向重叠至少 45%、
  平行方向间隙不超过 96 像素时直接 union，随后才执行联合 ROI 复检、全局 NMS、ROI filter 和
  `SharedDetections::update()`。不再以 tile 物理外边缘作为首段融合条件。新增 smoke 断言验证两个
  4K tile 半框直接融合为一个框。交叉构建和 `git diff --check` 通过；45 秒板端复测日志
  `/tmp/alertgateway_4k_20260720_215644/board.log` 显示 `ROI boundary merges=317`、联合 ROI 320 次、
  RGA 失败 0，稳定窗口约 30 FPS/18 Mbps，`enc_drop/out_drop/write_fail=0` 且正常 `Done`。这证明
  Tracker 前融合路径已实际执行，仍需用户肉眼确认跨缝手机/笔记本是否不再重框。
- 2026-07-21 新增渲染前重叠审计：`EncodeThread` 在从 `SharedDetections` 取得最终显示快照后、实际绘制
  前，对全部框两两计算交集；检测到交集面积大于 `stream.render_overlap_min_intersection_area_px`（默认 1）时
  输出 `[RenderOverlap]`，包含通道、视频/检测帧和 PTS、两框类别/置信度/坐标、交集矩形与面积、IoU 和
  IoS。该机制默认开启（`stream.render_overlap_audit=true`），且同一检测快照只记录一次以避免编码帧重复刷屏；
  它完全只读，不改变 NMS、Tile 融合、Tracker 或渲染内容。4K ROI 候选配置已显式开启，下一步应部署并根据
  `[RenderOverlap]` 日志区分同类/异类及框来源时序，再针对实际原因调整上游逻辑。

## 2026-07-21 SRS 测试前置与启动实例核验

- 主机只读核验确认当前 Windows SRS 健康：实际进程为
  `D:\Tool\SRS 5.0\SRS\objs\srs.exe -c conf\\console.conf`，TCP 1935 和 1985 均由该进程监听，
  `http://192.168.0.168:1985/api/v1/streams/` 返回 HTTP 200；当前无活动流。当前 Windows TCP
  排除范围已不包含历史记录中的 7998--8097，历史 8080 bind 问题属于此前配置状态，不应当作为当前
  监听事实继续使用。
- 当前单路/双路测试脚本不会启动或停止 SRS。`run_4k_candidate_on_board.sh` 在启动 FFmpeg 发布端、
  SSH 板端进程之前先检查 SRS API；没有 SRS 时的报错是有意的前置失败，脚本自身没有机会占用
  Windows 的 1935/1985 服务端口。脚本退出清理的对象仅是本次启动的 FFmpeg、板端 AlertGateway
  和本地 API 轮询子进程。
- Windows 存在两个不同启动入口：用户 Startup 中的 `SRS AutoStart.cmd` 使用 `console.conf`，
  SRS 自带 `srs-live.bat` 使用 `live.conf`；两者都监听 RTMP 1935 和 HTTP API 1985，不能同时运行。
  后续只保留一个规范启动入口，并用实际进程 CommandLine 判断生效配置，避免把重复 bind 误认为测试造成的故障。
- 同日复现：主动关闭 SRS 后直接执行当前 `run.sh`，脚本在 `1985` API 检查阶段等待约 5 秒后退出码为 1；
  此时尚未启动 FFmpeg 发布端、SSH 板端进程或 AlertGateway，且仅留下本次调试目录。该实测排除了“测试
  先占用 SRS 端口再导致桌面 SRS 启动失败”的因果关系。
- 用户提出待重启验证假设：开机后 SRS 自动启动尚未完成或尚未启动时直接执行测试，可能造成“测试先报错、
  随后手动启动 SRS 失败”的时间顺序假象。该假设尚未证实；下次重启后应先分别记录 `srs.exe` 进程、
  Windows TCP 1935/1985 监听和 API 返回状态，再执行测试，重点区分开机自启时序问题与重复启动实例问题。
- 2026-07-21 已按用户决定将测试流程改为“用户手动启动 SRS，测试脚本只检查和等待”。新增
  `tools/test/check_srs_manual.sh`：在任何 FFmpeg 发布端、板端 AlertGateway 或测试进程启动前，
  同时检查 SRS API `1985` 与 RTMP `1935`；未就绪时暂停等待用户手动启动后按 Enter 重试，输入 `q` 退出。
  4K、单进程多通道、双进程双路、独立发布和拉流重连入口均接入该闸门，脚本没有启动、重启或停止 SRS 的逻辑。
  Windows 启动项属于仓库外状态，本次未擅自修改；若要求完全禁止开机自启，仍需在 Windows 上手动禁用
  `SRS AutoStart.cmd`，并避免同时运行 `srs-live.bat`。
- 同日直接在未启动 SRS 的环境运行 `./run.sh` 验证：脚本完成本地视频 `ffprobe` 后停在 SRS 前置检查，
  明确报告 API `1985` 与 RTMP `1935` 均不可达，并等待手动启动/Enter 或 `q`；输入 `q` 后以预期的非零状态退出。
  期间未进入板端部署、FFmpeg 发布或 AlertGateway 启动阶段；仅清理阶段创建了 0 字节 `board.log` 证据文件。
- 随后用户手动启动 SRS 后再次运行 `./run.sh`，完成 120 秒单路 4K ROI/Tiling 实流验证，日志目录为
  `/tmp/alertgateway_4k_20260721_132506`。SRS 前置检查通过，输入 `live/testsrc2` 与结果
  `live/alertgateway` 均 active；SRS 快照确认输入 H.264 High 3840×2160、约 39--40 Mbps，输出
  H.264 Main 3840×2160、约 18 Mbps。稳定窗口约 30 FPS，`enc_drop=0`、`write_fail=0`、队列无持续积压，
  `ROI Tiling with 2 tiles`、边界合并和联合 ROI 抑制均实际执行；首个统计窗口有一次 `out_drop=1`，后续稳定窗口为 0。
  测试以退出码 0 完成，FFmpeg 与板端进程均由脚本清理，SRS 保持运行但结果流随后变为 inactive。日志仍出现
  `[RenderOverlap]` 审计记录，故本轮只能判定 4K 推理/编码/推流链路通过，不能宣称视觉重叠框问题已解决。

## 2026-07-21 ROI 重叠框定位与收敛

- 对 `/tmp/alertgateway_4k_20260721_132506/board.log` 的渲染前审计分类确认：问题同时包含同类重复轨迹和跨类别近同框误检。低 IoU 的手机/书本、手机/键盘以及笔记本/内置键盘等关系不能按 IoS 一刀切，否则会误删合法嵌套或相邻目标。
- `TilingTask::merge()` 新增类别无关的近同框抑制：仅当不同类别 IoU 至少 `0.85` 时保留高分框；同类别仍使用已有 NMS/包含规则。该规则放在 Tracker 前，并在 `InferThread` 输出 `cross-class near-duplicate suppression=N` 便于审计。
- `SharedDetections` 新增三层窄范围处理：同类别高 IoS 且中心包含时允许跨帧复用轨迹；跨类别 IoU 至少 `0.85` 时允许类别切换复用轨迹；更新末端合并已经形成的重复轨迹，并在最终显示快照再做同样条件的安全去重。低 IoU 框不受影响。
- `tools/benchmark/image_processing_smoke.cpp` 增加同类跨帧包含、跨类近同框、同一更新内重复轨迹和显示快照去重断言。交叉编译通过，ARM64 板端 `image_processing_smoke: OK`。
- 最新二进制已部署到 `firefly@192.168.0.200:~/AlertGateway/AlertGateway`。最后一轮 60 秒实流日志为 `/tmp/alertgateway_4k_20260721_140018/board.log`：`RenderOverlap=809`，同类别 `IoS>=0.70` 为 `0`，跨类别 `IoU>=0.85` 为 `0`；ROI 边界合并 437 次、联合 ROI 抑制 437 次、跨类别近同框抑制 53 次。稳定窗口约 30 FPS、约 18 Mbps，`enc_drop=0`、`out_drop=0`、`write_fail=0`，测试退出码 0。
- 仍会有低 IoU 的交集审计记录；它们不等同于重复框，后续若用户在播放器发现具体误叠，应以对应 `[RenderOverlap]` 坐标判断是否为真实相邻目标、模型误检或播放器旧缓存。当前测试只清理本次 FFmpeg/板端进程，SRS 未被测试启动、重启或停止。

## 2026-07-21 ROI 最终单框竞争逻辑

- 用户确认最终展示对同一位置的大小/类别竞争框只保留一个；不能按框面积或原始分数直接取舍，因为 tile 小框可能只是半个物体。实现保留候选来源：联合 ROI 复检优先于 tile，两个 tile 候选再优先离 tile 边缘更远者，最后才比较置信度。
- `TilingTask` 的边界融合结果不再丢弃 tile 元数据；联合 ROI 检测以 `JointRecheck` 来源加入。在 Tracker 前新增 `select_exclusive_candidates()`：跨类别近同框（IoU >= 0.85）或小框中心被大框包含且 IoS >= 0.50 时构成互斥组，只输出优先级最高的一个，并输出 `[Infer] ROI exclusive suppression=N`。
- `SharedDetections` 对同样的跨类别竞争关系不再立即切换类别：首帧把挑战者挂在既有轨迹上且不创建第二个框，连续两帧兼容后才受控切换；最终快照和重复轨迹清理也使用同一规则兜底。这样可避免手机/笔记本等大小框在相邻检测帧间来回跳动。
- `image_processing_smoke` 覆盖联合复检优先、强包含只保留一个框，以及跨类别第 1 帧保持旧框、第 2 帧才切换；交叉编译通过，已在 ARM64 板端执行并输出 `image_processing_smoke: OK`。最新二进制已部署到 `firefly@192.168.0.200:~/AlertGateway/AlertGateway`。
- 使用已手动启动的 SRS 进行 ROI 实流检查时，日志 `/tmp/alertgateway_4k_20260721_143208/board.log` 出现 77 次 `ROI exclusive suppression`，说明新候选互斥层已实际执行；链路统计稳定在约 30 FPS、约 18 Mbps，`enc_drop=0`、`out_drop=0`、`write_fail=0`。该轮旧的 0.60 IoS 版本仍记录到 IoS 约 0.50 的跨类别竞争框，因此门槛随后已收紧为 0.50 并重新完成板端冒烟测试；应在下一次播放器 A/B 中重点核对该边界案例。测试脚本只检查 SRS 是否已手动启动，未启动、停止或重启 SRS。

## 2026-07-21 双路推流实测

- 双路测试使用 `VID_20260712_131214.mp4` 和 `VID_20260712_131410.mp4`，源地址分别为 `live/dual_a`、`live/dual_b`，结果地址为固定模板 `live/alertgateway_channel_1`、`live/alertgateway_channel_2`。
- 首次多次前置失败的根因不是 SRS 未启动，而是测试主机环境设置了 `http_proxy=http://192.168.0.168:7897`；curl 访问 SRS API 时实际连接代理端口 7897，导致 1985 请求超时，而 RTMP 1935 正常。使用 `NO_PROXY/no_proxy=192.168.0.168,192.168.0.200` 直连后，API 返回正常并通过手动 SRS 检查。
- `/tmp/alertgateway_multi_20260721_160846/board.log` 记录单进程两路 `ChannelPipeline` 均启动并正常停止。稳定窗口两路约 29--31 FPS、约 7.9--8.3 Mbps，`enc_drop=0`、`write_fail=0`，仅早期 channel 2 出现一次 `out_drop=1`，后续为 0；推理队列的 `infer_drop` 属于低延迟最新帧覆盖计数。测试残留的板端进程已按本次 PID 优雅退出，SRS 未被测试启动、停止或重启。
- 随后再次使用直连 SRS、`RUN_SEC=60` 完成双路复测，日志为 `/tmp/alertgateway_multi_20260721_161133/board.log`。两路均通过 API 前置检查并激活结果流；不同窗口输出约 21--28 FPS、约 5.6--7.5 Mbps，后续两路均持续有 `enc_drop=0`、`out_drop=0`、`write_fail=0`，队列无持续积压。两路 `ChannelPipeline` 均输出 `stopped` 和 `Done`，板端进程已退出；日志末尾 FLV header 更新提示属于中断收尾提示，不影响 RTMP 输出统计。

## 2026-07-21 4K 非 ROI 性能拆分

- 使用 `config/config_4k_candidate_20260719.json`（继承 `config_4k_18mbps.json`，ROI/Tiling 关闭）完成 60 秒 4K 测试，日志为 `/tmp/alertgateway_4k_20260721_161456/board.log`；结果地址为固定单路 `rtmp://192.168.0.168/live/alertgateway`。
- 1,279 个推理样本：NPU 单次平均 `26.961 ms`，对应 NPU 原始处理能力约 `37.09 次/秒`；包含 CPU、拷贝、转换和调度的完整 `Infer` 平均 `40.942 ms`，实际完整推理节拍约 `24.42 帧/秒`。`infer_drop` 仍是低延迟队列覆盖计数，不是编码或推流丢帧。
- `StreamStats` 共 5 个窗口：包含启动阶段平均约 `29.34 FPS / 17.65 Mbps`；去掉首个启动窗口后，稳定约 `30.03 FPS / 18.07 Mbps`，所有窗口 `write_fail=0`、队列为 0。`EncodeStats` 的 `out_drop=0`，说明推流链路没有输出丢帧。

## 2026-07-21 四路并发推理压力实测

- 新增临时 4 路非 ROI 配置 `config/config_multi_4ch_candidate.json`，通道 1--4 分别拉取 `dual_a`--`dual_d`，结果固定输出到 `alertgateway_channel_1`--`4`。C/D 源流分别复用 A/B 视频，仅用于观察并发资源压力。
- `/tmp/alertgateway_multi_4ch_20260721_1624.log` 确认四个 `ChannelPipeline` 都启动并且四个结果流都产生过帧。每路独立统计：channel 1 NPU 平均 `50.44 ms`、完整推理 `55.06 ms`；channel 2 为 `50.72/55.24 ms`；channel 3 为 `50.31/54.90 ms`；channel 4 为 `48.23/52.71 ms`。对应每路按单次处理时间估算的服务率约 `19.7--20.7 次/秒`，但实际输出窗口约 `13--16 FPS`。
- 与单路 4K 的 NPU `26.96 ms`、完整推理 `40.94 ms` 相比，四路同时请求全部三个 NPU 核心后 NPU 时延明显上升，证明当前没有应用层调度，主要瓶颈是多 RKNN context 的全核争用。四路 `infer_drop` 持续增加，但各路 `enc_drop=0`、`out_drop=0`、`write_fail=0`；当前结论是四路 pipeline 可以运行，不能保证四路 30 FPS 检测。
- 随后为便于播放器同时观察，启动 5 分钟四路运行；实时日志 `/tmp/alertgateway_multi_4ch_20260721_1628.log` 显示争用继续恶化，部分时段每路 NPU 单次耗时升至 `85--88 ms`，因此画面明显卡顿。该压力进程已主动停止，SRS 未被操作；后续必须先引入全局 NPU 调度/限频，再重新做四路观看测试。

## 2026-07-21 全局 NPU 调度设计

- 已新增 `docs/NPU全局调度/NPU全局调度设计.md` 与可执行步骤
  `docs/NPU全局调度/NPU全局调度开发计划.md`，针对四路 all-core RKNN context 争用给出增量设计，尚未修改运行代码。
- 首版采用单个全局串行 NPU worker（独占一个 all-core RKNN context）和每通道最新帧邮箱；按限频后的 Round-Robin 公平派发，视频编码/推流保持独立。
- 按单路 NPU 26.961 ms、保留 20% 余量估算，全局安全预算初始为约 29 次/秒；四路等权默认约 7 次检测/秒/路。该数值是待实测校准的起点，不能宣称四路 30 FPS 检测。
- ROI/Tiling 将在基本调度验收后按单次 `rknn_run` 计 slot 接入，避免一个通道连续占用 NPU；SRS 仍只允许用户手动启动。

## 2026-07-21 全局 NPU 调度 P0 完成

- 已完成 P0（配置模型与接口边界），新增 `src/infer/NpuInferenceScheduler.hpp/.cpp`，目前只保存进程级调度配置和后续结果类型边界，不创建 worker、不调用 RKNN。
- `ModelConfig` 已新增 `target_infer_fps` 与 `npu_weight`，`ChannelPipeline` 和 `InferThread` 已预留同一个 Scheduler 的依赖注入接口；默认值保持旧的独立推理行为。
- `main.cpp` 已解析并校验 `npu_scheduler`：只接受 `global_serial` / `0_1_2`，检查预算、通道目标、模型路径/输出布局/输入契约一致性，并要求第一版关闭 Thumbnail/ROI/Tiling。P0 对 `enabled=true` 会在校验通过后明确拒绝启动，防止尚未实现 worker 时误走旧的多 context 并发路径。
- `AlertGateway`、`infer_camera_smoke`、`image_processing_smoke` 交叉编译通过；现有 JSON 配置已用 Python 标准库通过语法校验（环境没有 `jq`），`git diff --check` 通过。未上板、未启动测试流、未操作 SRS。
- 下一步：P1，实现可注入假 executor 的 Scheduler 邮箱、限频、Round-Robin 和主机侧算法测试，仍不接入 RKNN。

## 2026-07-21 全局 NPU 调度 P1 完成

- `NpuInferenceScheduler` 已实现独立于 RKNN 的单 worker 调度骨架：通道注册/注销、每通道一帧 latest mailbox、提交覆盖、过期帧淘汰、按 `target_infer_fps` 限频、从上次服务通道后开始的 Round-Robin 选路，以及安全停止时清空未派发邮箱。
- Scheduler 通过可注入 `Executor` 抽象执行 job；P1 只使用主机假 executor，未创建 RKNN context、未调用 RGA/RKNN、未接入 `InferThread` 的实际推理路径。完成回调在停止/注销后被抑制，避免生命周期重入。
- 新增 `tools/benchmark/npu_scheduler_smoke.cpp` 与 `BUILD_NPU_SCHEDULER_SMOKE`。主机原生编译运行输出 `npu_scheduler_smoke: OK`，覆盖最新帧覆盖、四路顺序 Round-Robin、7 FPS 限频、过期帧计数及停止丢弃待处理帧。
- 交叉编译 `AlertGateway`、`infer_camera_smoke`、`image_processing_smoke`、`npu_scheduler_smoke` 全部通过；`git diff --check` 通过。未上板、未启动测试流、未操作 SRS。
- 下一步：P2，把 RKNN context、输入内存、预处理、`rknn_run`、输出获取和后处理抽取为唯一 NPU executor；在此之前 `npu_scheduler.enabled=true` 仍会被主程序安全拒绝。

## 2026-07-21 全局 NPU 调度 P2 完成

- 已新增 `src/infer/RknnNpuExecutor.hpp/.cpp`。该对象独占 RKNN model context、`RKNN_NPU_CORE_0_1_2` mask、SRAM zero-copy 输入内存和输出 buffer 生命周期；`execute()` 在同一调用线程内完成 RGA（失败时 CPU）预处理、cache sync、`rknn_run`、outputs_get/release、decoded/rockchip_dfl 后处理，并只向上返回普通 `Detection` 列表和分段耗时。
- P2 仍未把 Executor 挂到 `NpuInferenceScheduler` 或替换现有 `InferThread` 旧路径，因此当前单路/多路运行行为不变，也不会误形成“已完成全局串行化”的结论。主程序对 `npu_scheduler.enabled=true` 的 P0 防护保持有效。
- `NpuInferenceResult` 已扩展为可承载检测结果、预处理、输入同步、NPU 和输出后处理耗时；P1 host scheduler smoke 仍通过。
- 交叉编译 `AlertGateway`、`infer_camera_smoke`、`image_processing_smoke`、`rknn_benchmark`、`npu_scheduler_smoke` 全部通过；现有 JSON 语法和 `git diff --check` 通过。未上板、未启动测试流、未操作 SRS。
- 下一步：P3 将 Scheduler 创建的唯一 `RknnNpuExecutor` 注入 worker，并把 `InferThread` 从直接 `rknn_run()` 改为提交/接收结果；需同时完成启动、停机和回调线程模型。

## 2026-07-21 全局 NPU 调度 P3 完成（未上板）

- `main.cpp` 在 `npu_scheduler.enabled=true` 时已创建唯一 `RknnNpuExecutor`、以它作为 `NpuInferenceScheduler` 的 executor 并启动 scheduler worker；多个 `ChannelPipeline` 注入同一 Scheduler。旧配置未开启该节点时仍保持原有独立 `InferThread` 路径。
- Scheduler 模式下 `InferThread` 不再加载模型或调用 `rknn_run()`：它只从原有输入队列取最新帧并提交 mailbox；完成回调在 scheduler worker 上更新 `SharedDetections`、输出分段耗时日志并执行 MQTT 节流上报。P3 的配置校验仍要求 Thumbnail/ROI/Tiling 全部关闭。
- 结果对象已补充源帧尺寸、PTS/时间戳，保证 Scheduler 回调更新的检测快照与编码对齐。Scheduler 的注销逻辑现在会标记通道不可再派发、清理 mailbox，并等待运行 job 和已开始的回调结束后再销毁通道状态；host smoke 新增该生命周期用例并通过。
- 已移除 P0 对合法 `npu_scheduler.enabled=true` 的无条件拒绝。此模式首次启动会只创建一个 all-core RKNN context；实际板端验证尚未执行，不能宣称多路卡顿已解决。
- 主机 `npu_scheduler_smoke: OK`；交叉编译 `AlertGateway`、`infer_camera_smoke`、`image_processing_smoke`、`rknn_benchmark`、`npu_scheduler_smoke` 全部通过，`git diff --check` 通过。未上板、未启动测试流、未操作 SRS。
- 下一步：P4 增加可读的全局/通道统计与独立四路 scheduler 候选配置，再进行 P5 的单路、双路、四路分层板端验证。

## 2026-07-21 全局 NPU 调度 P4 完成（未上板）

- Scheduler 已新增 10 秒滚动统计：全局输出派发/完成数、NPU 平均/P95、busy ratio、排队等待 P95、过期帧数；每通道输出目标/实际检测 FPS、mailbox 覆盖、限频、过期、等待与端到端 P95、失败数。空闲时 worker 也会按统计到期时间唤醒并输出，避免仅在有帧时才统计。
- `NpuInferenceResult` 与 scheduler job 记录提交、派发和完成时间，统计口径是 Scheduler 内部时间：queue wait 为提交到派发，e2e 为提交到 executor 返回；与已有 Encode/Stream 统计独立。
- 新增 `config/config_multi_4ch_scheduler_candidate.json`：四路 `dual_a`--`dual_d`，固定结果地址 `alertgateway_channel_1`--`4`，全局预算 29 FPS、各路目标 7 FPS、最大帧龄 250 ms、ROI/Tiling/Thumbnail 均显式关闭。它是候选配置，尚未部署或实测。
- 主机 `npu_scheduler_smoke: OK`；交叉构建和新候选 JSON 语法校验通过，`git diff --check` 通过。未上板、未启动测试流、未操作 SRS。
- 下一步：P5 按单路 Scheduler 回归、双路、四路 120 秒的顺序上板测试；SRS 只能由用户手动启动，测试前只做就绪检查。

## 2026-07-21 全局 NPU 调度 P5 真机回归通过

- 测试全程只使用用户已手动启动的 SRS：只读检查 API/流状态，不执行 SRS 的启动、停止或重启；候选二进制以 `~/AlertGateway/AlertGateway.npu_scheduler_p5` 独立文件运行，配置放在板端 `/tmp/`，未覆盖生产二进制或配置。
- 单路 Scheduler 60 秒回归：仅创建一个 `[NpuExecutor] RKNN context ready`，没有旧 `InferThread` 的独立 `rknn_init`。稳定窗口为 7.00--7.10 FPS，NPU P95 约 26.6--27.0 ms，队列等待 P95 约 102--119 ms、端到端 P95 约 144--150 ms；视频编码/推流约 30 FPS、约 8 Mbps，`enc_drop/out_drop/write_fail=0`。
- 双路 60 秒回归：仍只创建一个 RKNN context，总调度稳定约 14 FPS（两路各约 6.9--7.0 FPS），NPU P95 约 27.2 ms、busy ratio 约 43%、队列等待 P95 约 127--135 ms；两路编码和 RTMP 推流均无编码丢弃或写失败。
- 四路首轮使用四个本机实时缩放/编码发布端，输入仅约 14--23 FPS，调度实际约 22 FPS，故不作为验收结果；NPU 仍稳定在 P95 27.7 ms，证明瓶颈在主机发布端而非 Scheduler。
- 四路有效回归改为预转码 1080p 文件后以 `-c:v copy` 发布四路。70 秒候选运行正常 `Done`：全局实际约 28.0--28.4 FPS，四路分别约 6.99--7.10 FPS；NPU 平均约 26.57--26.73 ms、P95 27.51--27.78 ms、busy ratio 约 87--89%，无 stale drop/执行失败。四路输入、编码和结果 RTMP 均约 30 FPS、约 8 Mbps，最终稳定窗口所有通道 `put_fail=0`、`out_drop=0`、`write_fail=0`、队列为 0。
- 结论：P5 的单路、双路和四路验收已通过。全局单 worker + latest mailbox + 限频 Round-Robin 消除了原先四个 all-core RKNN context 争用导致的 85--88 ms NPU 尖峰；四路默认能力是每路约 7 FPS 检测、每路 30 FPS 编码推流，而不是四路 30 FPS 推理。
- 下一步：由用户人工同时观看固定结果地址 `rtmp://192.168.0.168/live/alertgateway_channel_1` 至 `_4`，确认实际播放流畅性；随后可根据观感或指标再校准 29 FPS 全局预算、权重策略或进入 ROI/Tiling 的 slot 化接入。

## 2026-07-21 当前 run.sh 默认 4K RGA/Zero-Copy 回归

- 用户手动启动 SRS 后直接运行 `./run.sh`，实际进入默认的单路 4K ROI/Tiling 配置 `config/config_4k_roi_candidate_20260720.json`，不是四路 Scheduler 配置；SRS 前置检查通过，输入与结果流均 active，测试脚本未启动、停止或重启 SRS。
- 120 秒测试日志为 `/tmp/alertgateway_4k_20260721_183855/board.log`。4K 输入/输出稳定约 30 FPS，输出约 17.8--18.4 Mbps；`enc_drop=0`、`out_drop=0`、`write_fail=0`，队列无持续积压，程序正常 `Done` 退出。
- ROI Tiling、ROI boundary merge、joint-ROI recheck 和 exclusive suppression 均持续执行；板端日志没有 `RGA path failed`、`RGA letterbox failed`、`RGA preprocess failed`、RKNN 执行或输出获取失败。该轮确认当前旧 InferThread 路径的 RGA 直写 RKNN 输入内存和 CPU fallback 机制可稳定运行；`infer_drop` 是低延迟最新帧覆盖计数，不是编码/推流丢帧。

## 2026-07-21 四路 4K Scheduler 测试入口准备

- 新增 `config/config_multi_4ch_scheduler_4k_candidate.json`：四路输入均声明为 3840x2160@30，使用全局串行 Scheduler、全局 29 FPS、每路目标 7 FPS；Thumbnail、ROI、Tiling 明确关闭，结果地址固定为 `alertgateway_channel_1` 至 `_4`。
- 新增可执行入口 `run_4k_4channel.sh`。脚本默认使用两段 4K 视频，C/D 分别复用 A/B；四路输入通过 `ffmpeg -c:v copy` 发布到独立 SRS 地址，避免主机实时转码成为测试瓶颈；板端以 `/tmp` 隔离二进制、配置和日志运行一个 Scheduler 进程。
- 脚本只调用 `check_srs_manual.sh` 检查用户手动启动的 SRS，继承固定 `NO_PROXY/no_proxy` 直连设置，不包含 SRS 启动、停止或重启逻辑；测试结果、SRS 快照和板端日志保存在 `/tmp/alertgateway_4k_4channel_<timestamp>/`。
- 已通过 `bash -n`、JSON 结构和四路分辨率/结果地址断言、`git diff --check`；尚未运行该四路 4K 入口。

## 2026-07-21 四路 4K 全帧 Scheduler 首次实测未通过

- 使用 `run_4k_4channel.sh`、四路原始 3840x2160 H.264 码流复制发布，测试日志为 `/tmp/alertgateway_4k_4channel_20260721_185916/board.log`。SRS 确认四路输入均为 H.264 High 3840x2160，四路结果均为 H.264 Main 3840x2160，因此第 2 路输入文件和 SRS 转发没有发现损坏。
- 四路同时进行 4K 解码、RGA 前处理、全局 NPU 推理和 4K MPP 编码时，整体资源已超载：NPU busy ratio 接近 99--100%，全局 NPU 平均约 31--33 ms、P95 约 44--47 ms；各路编码/推流实际约 12--19 FPS，第 2 路约 13--17 FPS，码率约 3.5--4.7 Mbps，而不是配置目标 30 FPS/8 Mbps。
- 第 2 路日志中 `enc_drop=0`、`out_drop=0`、`write_fail=0`，输入帧和输出包数量一致，也没有 RGA、RKNN、MPP 初始化或 RTMP 写失败；但播放端报告第 2 路花屏，当前应按“整体 4K 资源超载导致输出不满足实时编码条件”处理，不能判定四路 4K@30 可用。
- 当前 Scheduler 解决了 NPU context 争用，但不能解决四路 4K 解码/前处理/MPP 编码总负载；下一步应先分别测四路 4K 降到 15 FPS 的稳定性，或评估 4K 输入降分辨率输出/编码资源分配，再决定是否继续追求四路 4K@30。

## 2026-07-21 四路真实 4K@15 FPS Scheduler 测试

- 为避免把“输入 30 FPS、程序内部抽帧到 15 FPS”误当成 15 FPS 测试，先在主机生成了真实
  3840x2160@15 FPS 的 H.264 测试源：`/tmp/alertgateway_4k_4channel_15fps_sources/source_a_15fps.mp4`
  和 `source_b_15fps.mp4`，并以码流复制方式发布四路。有效测试日志为
  `/tmp/alertgateway_4k_4channel_20260721_191909/board.log`。
- 四路输入均为真实 4K@15 FPS，使用全帧 Scheduler，ROI/Tiling/Thumbnail 均关闭。全局调度实际约
  239--250 次/10 秒，NPU 平均约 26.9--27.3 ms、P95 约 28--29 ms，busy ratio 约 95--98%。
- 四路实际检测约 5.3--6.4 FPS；编码/推流约 8--11 FPS/路，未达到 15 FPS 目标。第 2 路稳定窗口约
  9.7--10.9 FPS、5.05--5.85 Mbps。各路稳定窗口 `enc_drop=0`、`out_drop=0`、`write_fail=0`，
  未发现 RGA、RKNN、MPP 初始化或 RTMP 写失败。
- 结论：真实四路 4K@15 FPS 仍不能判定为可用，瓶颈是四路 4K 解码、RGA 前处理、NPU 和 MPP
  编码的总体资源饱和；相比 4K@30 已明显改善，但 Scheduler 只能解决 NPU context 争用，不能
  将整条四路 4K 管线提升到实时 15 FPS。此前一次“15 FPS”测试使用的仍是 30 FPS 输入，不作为
  有效结论。

## 2026-07-21 4K 解码后降到 1080p 候选

- `PullStreamThread` 新增 `source.output_width/output_height`，在 FFmpeg 解码并整理为连续 NV12
  后优先调用 RGA 缩放；RGA 失败时使用 CPU 最近邻兜底。后续编码、推理和 RTMP 推流只接收缩放后的
  处理帧，输入源的 `source.fps=30` 保持不变，不在该路径主动抽帧。
- 新增候选配置 `config/config_multi_4ch_scheduler_4k_to1080_candidate.json`：四路输入仍声明为
  3840x2160@30，解码后处理/输出为 1920x1080@30，Scheduler 全局预算 29 FPS、每路检测目标 7 FPS，
  结果地址仍为 `alertgateway_channel_1` 至 `_4`。
- 新增入口 `run_4k_4channel_to1080.sh`，复用原四路发布和手动 SRS 检查流程；交叉编译、JSON 和
  shell 语法校验已通过。当前尚未完成板端实测：本轮检查时 SRS API 与 RTMP 端口均不可访问，
  测试未启动任何 SRS 实例。

## 2026-07-21 4K 解码后降到 1080p 首次实测

- 用户手动启动 SRS 后，使用 `RUN_SEC=60 CONFIG=config/config_multi_4ch_scheduler_4k_to1080_candidate.json
  ./run_4k_4channel.sh` 完成四路测试；SRS 只做了 API/1935 只读前置检查，未被测试启动、停止或重启。
  日志位于 `/tmp/alertgateway_4k_4channel_20260721_195715/`。
- 板端日志确认四路均为 `3840x2160 yuvj420p` 解码后执行 `post_decode_resize=3840x2160 -> 1920x1080`，
  MPP 四路编码配置也确认为 `1920x1080`、目标 `30 FPS`。
- 四路并发时 RGA 的 NV12 缩放调用持续失败并进入 CPU 兜底（日志统计 `PullStream` RGA 缩放失败 81 次，
  Scheduler Executor 前处理 RGA 失败 31 次）。因此虽然处理分辨率已降为 1080p，但 4K→NV12 整理和 CPU
  缩放仍成为瓶颈，实际输出约为 channel 1 `14--16 FPS`、channel 2 `14--19 FPS`、channel 3 `10--16 FPS`、
  channel 4 `10--15 FPS`，没有保持 30 FPS。
- Scheduler 全局约 `208--245` 次/10 秒，单路实际检测约 `5.2--6.2 FPS`，NPU 平均约 `31--37 ms`、P95
  约 `41--65 ms`，busy ratio 约 `98--99%`。各路 `enc_drop=0`、`out_drop=0`、`write_fail=0`，说明没有
  发现 MPP 输出丢帧或 RTMP 写失败；本次结论是“分辨率配置正确，但 RGA/CPU 前处理与四路 4K 解码总负载仍不满足 30 FPS”。

## 2026-07-21 RGA 缩放失败原因定位

- 读取本次测试对应板端内核日志后，失败点明确发生在 RGA buffer map/submit 阶段，而不是缩放比例或
  1080p 格式不支持：`RGA_MMU unsupported memory larger than 4G`、`scheduler core[4] unsupported
  mm_flag[0x8]`、`src channel map job buffer failed`、`failed to map buffer`。
- 当前实现将 FFmpeg 解码后的 4K NV12 和 1080p 输出放在 `std::vector<uint8_t>`/`FrameBufferPool` 的
  CPU 虚拟地址中，再用 `wrapbuffer_virtualaddr()` 交给 RGA；四路新缩放路径同时使用“虚拟地址→虚拟地址”，
  当前板端 RGA MMU/驱动不能映射这类超过 32-bit 可寻址范围的用户态 buffer，因此 `improcess()` 返回 0。
  同一轮 Scheduler Executor 的 RGA 前处理也出现相同映射错误，说明不是单个缩放矩形参数的问题。
- 因此 CPU 兜底被频繁触发，4K NV12 整理和 CPU 缩放把四路链路拖到约 10--19 FPS。根本修复方向是让
  RGA 输入/输出使用可导入的 DMA-BUF/DRM/MPP buffer 并通过 `wrapbuffer_fd()` 或 handle 提交；仅增加
  RGA 互斥锁不能解决当前的地址映射错误，并发调度应在 buffer 路径修正后再评估。

## 2026-07-21 4K 读取路径改为 MPP NV12 + CPU 2:1 缩放

- 已在 `PullStreamThread` 中接入板端 `h264_rkmpp/hevc_rkmpp` 硬解码，并让 FFmpeg `get_format` 优先选择
  `AV_PIX_FMT_NV12`。旧版 FFmpeg 若不提供 NV12，仍保留 DRM_PRIME/DMA-BUF 兼容分支；默认 4K 候选走
  NV12 平面读取，不再把 DMA-BUF RGA 映射作为必经路径。
- `source.output_width/output_height=1920x1080` 时，读取线程直接从硬解码器 NV12 的 Y/UV 平面做 4K→1080
  精确 2:1 抽样，避免整张 4K NV12 临时拷贝和逐像素整数除法。输出 Frame 为连续 1080p NV12，后续推理、
  MPP 编码和 RTMP 推流保持原有路径。
- DMA-BUF 分支的 RGA 调用已尝试固定到 RGA3；但板端实际仍报 `Failed to map attachment`，并出现
  `rga2 ... swiotlb buffer is full`，说明四路 4K DMA-BUF 导入会耗尽当前系统映射资源。该分支不作为本次
  4K 主路径，避免持续重试污染内核 swiotlb；普通 1080p Frame 的 RGA 预处理仍可用。
- 交叉编译已通过。用户手动启动 SRS 后完成三次 25 秒四路验证，最新日志为
  `/tmp/alertgateway_4k_4channel_20260721_204036/board.log`：四路均确认 `3840x2160, Format: nv12`，
  应用日志没有新的 RGA/DMA-BUF/swiotlb 错误，四路结果流均 active，`enc_drop=0`、`out_drop=0`、
  `write_fail=0`，程序正常 `Done`。
- 最新稳定窗口实际编码/推流约为 channel 1 `16.6 FPS`、channel 2 `15.3 FPS`、channel 3 `16.5 FPS`、
  channel 4 `15.4 FPS`；Scheduler NPU 平均约 `26.6 ms`、P95 `28.3 ms`、busy ratio `85%`，各路检测约
  `6.8--6.9 FPS`。因此本次修改解决了 RGA DMA-BUF 映射错误和花屏风险，但四路 4K 输入仍未达到 30 FPS，
  当前剩余瓶颈在四路硬解码/CPU 4K 读取缩放/编码总吞吐，不能宣称四路 4K 实时通过。
- 下一步建议：先按 15 FPS 输入或单路/双路阶梯测试确认解码吞吐上限；若必须四路保持 30 FPS，应进一步
  采用 MPP/RGA 原生 DMA32 buffer pool 或在输入侧使用硬件媒体管线完成缩放，不能继续依赖当前板端四路
  DMA-BUF RGA 导入方式。

## 2026-07-21 最新一次四路 4K→1080 启动验证

- 使用 `RUN_SEC=30 CONFIG=config/config_multi_4ch_scheduler_4k_to1080_candidate.json ./run_4k_4channel.sh`
  再次启动验证，日志为 `/tmp/alertgateway_4k_4channel_20260721_204316/`。SRS 仍由用户手动启动，
  测试只做前置检查；四路输入和 `alertgateway_channel_1` 至 `_4` 结果流均 active，程序正常 `Done`。
- 稳定窗口中四路编码/推流约为 `16.5/16.0/16.5/14.9 FPS`，全局 NPU 平均约 `26.9 ms`、P95 约
  `28.2 ms`、busy ratio 约 `85--88%`；各路无编码丢帧、推流写失败或新的 RGA DMA-BUF 错误。
- 结论未改变：当前修改已使四路测试稳定运行并消除本次 RGA 映射故障，但四路 4K→1080 仍不能达到
  30 FPS；后续应继续从硬件媒体缩放/解码带宽方向优化，不能仅继续调 NPU Scheduler。

## 2026-07-21 四路脚本默认配置修正

- `run_4k_4channel.sh` 的默认 `CONFIG` 已从旧的四路 4K 全分辨率候选切换为已验证的
  `config/config_multi_4ch_scheduler_4k_to1080_candidate.json`。现在在 SRS 已手动启动的前提下，
  直接执行 `./run_4k_4channel.sh` 即会启动四路 4K 输入、4K→1080 处理、全局 Scheduler 和四路结果推流，
  不需要额外设置 `CONFIG` 环境变量。
- 同步修正脚本启动日志和 `run_4k_4channel_to1080.sh` 注释，明确当前路径是 MPP 硬解码/NV12 2:1
  缩放，不再误称为 RGA 缩放或 4K 全帧处理。
- 已在不设置 `CONFIG` 的情况下直接执行 `RUN_SEC=20 ./run_4k_4channel.sh` 验证默认入口：默认配置正确选中，
  四路输入与四路结果流均 active，板端进程正常 `Done`，无编码丢帧、推流写失败或新的 RGA 错误。

## 2026-07-21 四路 4K→1080 卡顿瓶颈复核

- 最新默认入口日志 `/tmp/alertgateway_4k_4channel_20260721_204636/board.log` 与四个发布端日志共同确认：主机四路输入均以约 30 FPS、约 40--44 Mbps 码流复制发布；板端 Pull 完成率却仅为 17.21/15.70/12.61/11.92 FPS，且其后 Encode 与 Stream 的帧率逐路一致，`enc_drop/out_drop/write_fail=0`、队列近零。因此卡顿发生在编码前，不是 SRS 网络、MPP 输出队列或画框开销造成。
- 该 Pull 指标覆盖 RTMP 读取、`h264_rkmpp` 解码、CPU 可读 NV12 输出和 4K→1080 CPU 2:1 抽样，现有统计尚不能把解码和缩放分别量化；已确认的瓶颈范围应准确表述为“4K 解码后 CPU 可读帧路径”，不能仅凭现有数据把全部责任断言为 CPU 缩放。
- `EncodeThread` 当前用 `frame_idx * 1000 / cfg_.fps` 生成 H.264 PTS，`cfg_.fps` 固定为 30。实际只生产 12--17 FPS 时，输出媒体时间仍每帧前进约 33 ms，和真实到包节拍不一致；这是播放端更明显卡顿/赶帧的独立次要问题，但不是吞吐下降根因。
- NPU 是另一条独立限制：全局约 27.7 FPS、每路约 6.9 FPS，故移动目标框约每 143 ms 才更新一次；它会造成框的阶梯感，但因编码输入本身已只有 12--17 FPS，不能解释整幅视频的低帧率。
- 下一步优先补 Pull 分段计时（读包、送包/取解码帧、4K→1080 缩放、两队列投递）并在同一输入下做“无缩放 4K 拉取”和“单/双/四路”阶梯 A/B；依据结果选择硬件媒体缩放或输入侧降至 1080p。若短期以 15 FPS 输出为目标，还需让编码 PTS 与实际帧率/源 PTS 一致，避免播放器额外的时间轴抖动。

## 2026-07-21 卡顿分析第一轮修改

- `PullStreamThread` 的 4K→1080 精确 2:1 NV12 抽样在 ARM 上增加 NEON `vld2/vld4 + vst1/vst2` 路径，保持原 nearest-neighbor 映射和 CPU 兜底路径不变；目标是降低逐字节标量循环的 CPU/内存访问开销，不重新启用已验证会触发 `swiotlb` 映射耗尽的 DMA-BUF RGA 主路径。
- Pull 统计新增 `read_avg_ms`、`decode_send_avg_ms`、`decode_receive_avg_ms`、`resize_avg_ms`、`enqueue_avg_ms`，用于下一次板端日志精确切分 RTMP 读包、MPP 解码、CPU 缩放和队列背压；当前尚未取得修改后的板端实测数据。
- 编码与推流时间轴改为沿用 `Frame::pts_ms`/MPP PTS，并以毫秒为 Stream 时间基，不再使用固定 `pkt_idx_` 宣称 30 FPS；输出 PTS 在每次 RTMP 连接建立后的首个包处归零。该修改只修正实际低帧率时的播放节奏，不会增加处理吞吐。
- 本轮交叉编译 `AlertGateway` 通过，`git diff --check` 通过；尚未提交或推送。
- 修改后短测前置检查于本轮返回 `192.168.0.168:1985` connection refused，因此遵守手动 SRS 约定未启动测试发布端或板端进程；板端性能数据待用户手动启动 SRS 后补测。

## 2026-07-21 卡顿分析修改后四路实测

- 用户手动启动 SRS 后，直接运行 `RUN_SEC=20 ./run_4k_4channel.sh`，日志为 `/tmp/alertgateway_4k_4channel_20260721_212522/board.log`；四路输入和四路结果均 active，程序正常 `Done`，测试未启动/重启/停止 SRS。
- 新增 NEON 路径后的 Pull 分段统计显示四路 `resize_avg_ms` 仅 `1.22--1.26 ms`，`enqueue_avg_ms=0.01 ms`；因此 CPU 4K→1080 缩放和队列背压不是当前主要瓶颈。`decode_send_avg_ms` 为 `6.18/9.92/7.14/9.29 ms`，`decode_receive_avg_ms` 为 `5.37/5.82/5.47/5.54 ms`，结合四路输出仅 `15.66/12.37/17.35/11.51 FPS`，瓶颈进一步收敛到四路并发 MPP 硬解码/共享媒体内存资源竞争及其与其他硬件处理的竞争。
- NPU 仍稳定为全局约 `27.6 FPS`、各路约 `6.89 FPS`，`npu_p95` 约 `27.8 ms`；编码/推流无失败，说明本轮没有发现 NPU 执行、MPP 编码或 RTMP 写入错误。
- 新时间戳链路已反映实际到帧间隔，例如 channel 2 的 `pts_delta_ms=118`，不再把低帧率帧伪装成固定 30 FPS；该项改善播放时间轴，但没有把四路吞吐提升到 30 FPS。
- 结论：NEON 优化验证了缩放不是主瓶颈；下一步不应继续在 CPU 缩放循环上投入，优先做单路/双路/四路硬解阶梯测试，或改用输入侧 1080p/硬件媒体缩放，以确认四路解码资源上限。

## 2026-07-21 1080p 四路脚本复核

- `run_1080p_4channel.sh` 当前使用 1080p 本地文件，但输入 RTMP 地址和等待检查名称仍是 `alertgateway_4k_source_a` 至 `_d`，与四路 4K 脚本复用。脚本的 `wait_stream` 只判断名称 active，不校验 SRS 返回的宽高、发布连接或本次发布者，因此存在残留/并发 4K 发布端导致串流的风险；应改为独立的 `alertgateway_1080p_source_a` 至 `_d` 地址，并在 active 检查中校验 1920x1080。
- 已有 `/tmp/alertgateway_1080p_4channel_20260721_214753/board.log` 表明本次实际解码为 `1920x1080 nv12`，不是误拉 4K；前约 10 秒输入/输出仅约 12--14 FPS，随后稳定窗口四路约 29.8--30.4 FPS，NPU 各路约 6.9--7.0 FPS、编码/推流无失败。因此“持续不顺畅”不能仅由板端四路 1080p 吞吐解释，需同时排查播放器多路解码/显示和启动阶段 SRS/GOP 缓冲观感。
- 已按方案修正 `run_1080p_4channel.sh` 与 `config/config_multi_4ch_scheduler_1080p_candidate.json`：输入流改为独立的 `alertgateway_1080p_source_a` 至 `_d`，脚本等待输入 active 时强制校验 SRS 返回的宽高为 `1920x1080`；四路结果地址保持固定 `alertgateway_channel_1` 至 `_4`。`bash -n`、JSON 断言和 `git diff --check` 通过，尚未重新上板测试。

## 2026-07-21 1080p 30 FPS 观感不顺的流程定位

- 当前视频链路没有常规意义上的主动抽帧：1080p 配置 `source.fps=30`、输入源/目标均约 30 FPS 时 `frame_step=1`，`pace_drop=0`；`enc_push` 与 `decoded` 一致，推理队列的 `push_latest` 只覆盖推理帧，不影响编码视频。稳定窗口 `enc_drop/out_drop/write_fail=0` 时也没有证据表明编码或 RTMP 丢帧。
- 当前 `PullStreamThread` 在解码后用 `steady_clock::now()` 写入 `frame_obj.pts_ms`，见 `src/capture/PullStreamThread.cpp` 的帧构造处；该时间包含解码线程调度抖动，不是输入视频的媒体 PTS。随后 `EncodeThread` 把它传给 MPP，`StreamThread` 又按毫秒时间基直接写入 RTMP。因而即使计数平均 30 FPS，输出帧的 PTS 间隔可能不均匀，播放器在时间戳空洞处保持上一帧，主观上会像“抽帧后插前帧”。
- 之前固定 `pkt_idx_` 按 30 FPS 生成均匀 PTS，可能掩盖了这种处理节拍抖动；最近改成实时 `Frame::pts_ms` 后，低负载 1080p 场景的时间轴观感可能反而变差。`EncodeStats` 中的 `pts_delta_ms` 是“当前视频帧与最近检测结果的年龄”，不是相邻视频帧 PTS 间隔，不能用它判断视频是否均匀。
- 修复方向：Pull 应优先保存 `AVFrame::best_effort_timestamp` 按输入 `AVStream::time_base` 换算出的媒体 PTS，并对无效/倒退 PTS 用稳定的 `1/fps` 递增值兜底；`timestamp_ms` 单独保留墙上时钟供运行时事件使用。之后应对 RTMP 输出抓帧/包做 PTS delta 和连续画面重复率 A/B，确认时间轴修复后再决定是否调整多路播放器缓冲。

## 2026-07-21 媒体 PTS 时间轴修复

- `PullStreamThread` 现在优先将 `AVFrame::best_effort_timestamp` 按输入视频 `AVStream::time_base` 换算为毫秒媒体 PTS；无效、倒退或首次缺失时按源帧率和 `frame_step` 使用固定帧间隔兜底。`Frame::timestamp_ms` 单独继续使用墙上时钟，避免媒体时间和事件时间混用。
- `StreamThread` 增加输出 PTS 单调保护，RTMP 重连或源 PTS 回退时按配置帧率补齐最小步进；`Frame/Encode` 注释已同步为媒体 PTS 语义。
- 交叉编译 `AlertGateway`、`infer_camera_smoke`、`image_processing_smoke`、`npu_scheduler_smoke` 全部通过，`bash -n` 和 `git diff --check` 通过。修改后 1080p 短测因 SRS API/RTMP 不可访问而未启动，未自动启动或重启 SRS。

## 2026-07-21 修改后四路 1080p 部署状态

- 再次确认当前 `192.168.0.168:1985` API 和 `1935` RTMP 均不可连接，故 `RUN_SEC=20 ./run_1080p_4channel.sh` 在 SRS 前置检查阶段退出；没有启动发布端或板端 AlertGateway 测试进程。
- 为便于 SRS 恢复后直接测试，已将当前交叉编译产物和 `config/config_multi_4ch_scheduler_1080p_candidate.json` 部署到板端临时路径：
  `/tmp/AlertGateway.1080p_4channel_20260721_220838` 和
  `/tmp/alertgateway_1080p_4channel_20260721_220838.json`。本次未启动、停止或重启任何板端进程；正常测试脚本仍会使用自己的隔离临时路径并负责测试结束后的清理。
- 用户已处理并解决本次 SRS 不可连接导致的测试阻塞问题。后续测试前仍只检查 SRS 是否已由用户手动启动，脚本不自动启动、重启或停止 SRS；尚待下一次测试运行验证连通性。

## 2026-07-21 4K 经 SRS 转码为 1080p 的方案决策

- 用户确认源视频保持 4K，先以原码流推送到 SRS；推荐由 SRS 所在主机联动 FFmpeg，将每路 4K 拉流缩放并编码为 1080p，再发布为独立的 `alertgateway_1080p_source_a` 至 `_d`，板端只拉取和解码 1080p。
- 推荐链路为：`4K源 -> SRS 4K原码流 -> 主机端FFmpeg转码1080p -> SRS 1080p中间流 -> 板端MPP解码1080p -> NPU推理 -> 1080p输出SRS`。SRS负责媒体转发和调度FFmpeg转码，不是自身直接完成像素缩放；可使用SRS官方 `transcode` 配置，或由外部脚本管理FFmpeg进程。
- 板端当前 `run_4k_4channel.sh` 路径是另一种方案：SRS转发4K原码流，板端 `h264_rkmpp/hevc_rkmpp` 先完整解码4K，再执行解码后NV12缩放到1080p。因此缩放虽降低NPU、编码和输出带宽压力，但不能绕过四路4K MPP/VPU解码瓶颈。
- 接入SRS端转码结果时，不能直接同时运行会向同一 `alertgateway_1080p_source_*` 地址发布本地测试文件的 `run_1080p_4channel.sh`；需要使用板端专用启动方式或增加跳过本地发布端的模式，避免同一流名出现多个发布者。

## 2026-07-21 当前代码冗余审查

- 只读审查确认交叉编译相关改动的 `git diff --check` 通过，未发现可直接删除的整段生产链路；当前 `PullStreamThread` 的 DRM_PRIME/RGA 分支虽然不是最新板端 NV12 主路径，但仍是旧版 Rockchip FFmpeg 输出格式的兼容回退，不应在未确认平台范围前删除。
- 已确认两个明确的清理候选：`StreamThread::pkt_idx_` 仅在重置处读写，实际输出 PTS 已完全使用源媒体时间戳；`Frame::rgb_data` 在工程内没有读写调用，注释也标明暂未使用。`Frame::mutable_data()` 当前同样没有仓内调用，但属于公共数据访问接口，是否删除需结合外部调用方确认。
- `PullStreamThread::output_buffer_group_` 当前在所有硬件解码通道启动时创建，但实际板端优先选择 NV12，只有收到 DRM_PRIME 且需要缩放时才使用该组；后续可将其改为 DRM 分支内惰性创建，减少当前NV12路径的无用资源申请。`hardware_decode` 字段注释也应同步为“优先NV12，DRM_PRIME兼容回退”。
- `run_4k_4channel_to1080.sh` 是对 `run_4k_4channel.sh` 的薄封装别名，暂不删除；多份候选 JSON 也暂不按文件名删除，需结合实际测试记录和用户后续使用情况确认。

## 2026-07-21 修改总览可视化文档

- 新增 `docs/本次修改总览.html`，用自包含HTML/CSS流程图展示本次主要改动：4K原码流到板端1080p处理链路、全局NPU Scheduler、多路输入输出管线、媒体PTS修复、问题与性能结论，以及SRS主机端FFmpeg转码方案对比。
- HTML已完成基础结构检查和 `git diff --check`；文档只读展示项目状态，不改变运行配置或测试行为。
