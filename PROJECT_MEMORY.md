# AlertGateway 项目记忆

最后更新：2026-07-08

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

- Git分支：`master`，本地已提交 `b190edb`，当前领先 `origin/master` 1 个提交。
- 工作区存在用户自己的未提交修改和未跟踪文档，处理任务时必须保留，不能覆盖或清理。
- 本地已有ARM64构建产物 `build/AlertGateway`。
- 仓库内 `model/` 只有 `.gitkeep`；实际运行所需的 `model/yolov8s.rknn` 未纳入Git。
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
- EXP-001至EXP-006详细操作与结果见 `docs/YOLOv8s-RK3588推理优化实验记录.md`。
- 测试时已经关注CPU/NPU governor；CPU频率会明显影响 `rknn_run()` 的同步耗时。
- 当前摄像头可以任意朝向用于测量纯 `rknn_run()`，因为固定输入尺寸下的稠密卷积计算量
  基本不随画面内容变化。
- 正式比较模型版本时，应改用同一个固定输入，预热100次、测试至少1000次，并记录中位数
  和P90。
- Rockchip Model Zoo当前公开的RK3588 YOLOv8s INT8数据为40.8 FPS，折算约24.5 ms，
  且标注为单核口径。网上约20 ms的数据不能在模型图、输入尺寸、工具链和测试环境不同的
  情况下直接比较。

## 已确定的优化原则

优化按照从简单到困难的顺序进行：

1. 统一官方模型与当前模型的benchmark口径；
2. 对比RKNN `optimization_level=0/1/2/3`；
3. 采用Rockchip官方优化检测头，将DFL和部分后处理移出NPU；
4. 将模型输入从640×640改为匹配摄像头比例的640×480；
5. 再评估576×448和512×384；
6. 建立桌面六类业务验证集并微调；
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

- README存在配置示例和实现细节不一致。
- CMake硬编码 `/home/rambos/sysroot`，声明的 `RKNN_LIB_PATH` 当前没有用于实际链接。
- 交叉编译工具链文件实际名称是`cmake/aarch64-toolchain.cmake`；旧文档中的
  `cmake/aarch64-linux-gnu.cmake`不存在，已修正开发指南。

## 下一步

按照推理优化路线，进入下一优化阶段：将模型输入尺寸从 640x640 改为匹配摄像头比例的 640x480 并建立对应校准、转换与后处理映射，重新在 Host 仿真评估其精度与推理效率表现。
