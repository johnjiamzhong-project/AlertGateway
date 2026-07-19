# AlertGateway

基于 RK3588S 的智能边缘网关，面向 4K 视频流的实时目标检测与告警。

边缘端 C++ 应用，运行在 Firefly ROC-RK3588S-PC 开发板上。视频来源支持本地 V4L2 摄像头与 RTMP/RTSP 拉流（SRS 转发）两种可配置模式，实现视频采集/拉流、NPU 推理、检测结果画框、硬件编码推流、MQTT 上报的完整业务闭环。推理侧支持三类可配置图像处理任务：Thumbnail 缩略图、ROI 区域追踪、ROI Tiling 小目标增强。

经过多轮性能调优，当前生产配置使用 Rockchip 官方九输出 YOLOv8s INT8 模型，NPU 阶段中位数约 26.7 ms（performance 锁频），后处理中位数 2.86 ms（L1/L2 缓存优化），完整推理链路约 30.65 ms。编码推流稳定运行在视频源实际帧率，RTMP 端到端延迟 < 1s，支持断线自动重连。

---

## 系统架构

```
┌──────────────────────────────────────────────────────────┐
│                     AlertGateway                          │
│                                                           │
│  视频源（可配置）                                          │
│  V4L2 本地摄像头   ─┐                                     │
│  RTMP/RTSP 拉流    ─┤──→  推理线程（含图像处理任务）        │
│  （SRS 转发）       │      RKNN NPU / YOLOv8s             │
│                     │      · Thumbnail 缩略图              │
│                     │      · ROI 过滤 / 追踪               │
│                     │      · ROI Tiling 小目标增强          │
│                     │               │                     │
│                     │     SharedDetections                 │
│                     │               ↓                     │
│                     ├──────→  编码线程  →  推流线程         │
│                     │        MPP H.264    RTMP 推流        │
│                     │        叠加检测框                     │
│                     └──────→  MQTT 线程                    │
│                              检测结果上报                   │
└───────────────────────┬──────────────────────────────────┘
                        │
           ┌────────────┴────────────┐
           ▼                         ▼
     SRS（主机）               MqttMonitor（主机）
     RTMP 转发 / 4K 中继        检测结果实时显示
           │
           ▼
     播放器（主机）
     拉流播放（含检测框画面）
```

---

## 功能说明

### 核心业务
- 支持 V4L2 本地摄像头与 RTMP/RTSP 拉流（SRS 转发）两种视频来源，通过 `source.type` 配置切换
- YOLOv8s NPU 推理，检测画面中的目标物品（手机、杯子、键盘等）
- 检测结果叠加画框（类别 + 置信度）后硬编码推流到 RTMP 服务器
- 检测结果（物品种类、数量、置信度）通过 MQTT 周期上报到上位机

### 可配置图像处理任务

三类任务可独立开启，默认全部关闭，不影响基础链路：

| 任务 | 说明 |
|------|------|
| **Thumbnail 缩略图** | 检测后生成指定尺寸 NV12 缩略图并附加到 MQTT payload（当前为 CPU 正确性路径，RGA/JPEG 为后续优化）|
| **ROI 区域追踪** | 圈定感兴趣区域（归一化坐标），仅保留 ROI 内目标，支持停留时长追踪与进出事件上报；代码已实现，板端专项验证待完成 |
| **ROI Tiling 推理** | 对 ROI 内区域做网格切分分别推理，弥补 4K 缩放后小目标漏检，结果合并全局 NMS；代码已实现，板端专项验证待完成 |


### 检测目标类别

基于 COCO 预训练的 YOLOv8s 模型，关注以下桌面常见物品类别（可在配置文件中调整）：

| 类别（英文） | COCO ID | 说明 |
|------|------|------|
| cell phone | 67 | 手机 |
| cup | 41 | 杯子 |
| keyboard | 66 | 键盘 |
| mouse | 64 | 鼠标 |
| laptop | 63 | 笔记本电脑 |
| book | 73 | 书本 |

### 4K 场景准确率提升

对于真实 4K 场景中杯子、书类/说明书、笔记本的漏检或框不贴合，应从已有 4K 测试视频按 1 FPS
抽帧、筛选、人工标注，并在 WSL/Linux GPU 环境微调后再进行 INT8 转换。量化校准只保证训练后
模型的 RKNN INT8 转换质量，不能替代标注和微调，也不能单独提升识别率。完整可执行流程见
[4K 准确率微调与 INT8 校准](docs/4k/4k_accuracy_finetune_and_int8_calibration.md)。

### 检测结果处理

替代原有的离岗/睡岗告警状态机，逻辑更简单：

1. 推理线程（`InferThread`）对每帧画面做目标检测，按 `target_classes` 过滤出关心的物品，结果写入共享结构 `SharedDetections`
2. 编码线程（`EncodeThread`）每帧编码前从 `SharedDetections` 读取最新检测结果，将检测框 + 类别 + 置信度叠加到画面上，再交给硬编码器
3. 推理线程按固定周期（如 1 秒，`report_interval_sec` 可配置）汇总当前帧检测到的物品种类与数量
4. 与上一次汇总结果对比，发生变化（新增/消失/数量变化）时通过 MQTT 上报一条 JSON 消息

上报消息示例：

```json
{
  "timestamp": 1718000000,
  "objects": [
    {"label": "cell phone", "count": 1, "score": 0.87},
    {"label": "cup", "count": 1, "score": 0.91},
    {"label": "keyboard", "count": 1, "score": 0.95}
  ]
}
```

---

## 性能优化与工程特点

### Pipeline 并行解耦
采集/拉流、推理、编码三条线程通过独立队列解耦：视频源同时向编码队列（阻塞、不丢帧）和推理队列（非阻塞、可丢帧，始终推理最新帧）推送数据，推理速度不决定编码推流帧率，编码线程稳定运行在视频源帧率。推理结果通过线程安全的共享结构传递给编码线程叠加画框，两条线程互不阻塞。

### NPU 推理性能
- **Rockchip 官方九输出 INT8 模型**：生产配置默认使用 `model/yolov8s_rockchip_dfl.rknn` 和 `model.output_layout=rockchip_dfl`，在 CPU 端完成 DFL、坐标解码和 NMS
- **后处理循环优化**：重构类别筛选内层循环为顺序内存访问，改善 L1/L2 缓存命中，后处理中位数从 4.14 ms 降至 2.86 ms（降幅 31%）
- **Zero-copy 输入 + RGA 硬件预处理**：视频源 YUYV / NV12 帧通过 RGA 一次性完成颜色空间转换与缩放，直接写入 NPU DMA 输入缓冲区
- 各实验详情见 `docs/YOLOv8s-RK3588推理优化实验记录.md`

### 视频叠框与标签
- 编码线程在 NV12 DRM Buffer 上直接绘制检测框和“类别 + 置信度”标签，不依赖 OpenCV
- 中文类别名仅用于视频绘制层，内部检测标签和 MQTT payload 仍保持英文 COCO 类别名
- `stream.draw_detection_labels` 可开关标签绘制，`stream.bitrate_kbps` 控制 MPP CBR 目标码率；当前生产建议为 2000 kbps

### 推流稳定性
- RTMP 推流参数（GOP、编码 profile、队列深度）针对低延迟场景调优，端到端延迟控制在 1 秒以内
- 推流连接具备自动断线重连能力；`PullStreamThread` 拉流模式同样支持断线重连

### 摄像头帧率自适应
根据 USB 摄像头在当前采集格式下的真实可达帧率配置采集参数，保证画面时间戳节奏与实际出帧速度一致，避免播放端缓冲异常。

---

## 技术栈

| 模块 | 技术 |
|------|------|
| 开发语言 | C++17 |
| 开发环境 | VSCode + Remote-WSL |
| 构建系统 | CMake |
| 交叉编译 | aarch64-linux-gnu-g++ 11.4.0 |
| 视频采集 | V4L2（本地）/ FFmpeg avformat+avcodec（RTMP/RTSP 拉流）|
| AI 推理 | RKNN Lite2 C API + YOLOv8s |
| 硬件编码 | Rockchip MPP（h264_rkmpp） |
| 图像处理 | RGA 硬件加速（格式转换 / 缩放 / crop）|
| 推流 | RTMP |
| 消息上报 | Paho MQTT C++ |
| 线程通信 | 有界阻塞队列 + 条件变量 |
| 配置管理 | JSON（nlohmann/json） |

---

## 目录结构

```
AlertGateway/
├── CMakeLists.txt
├── readme.md
├── config/
│   ├── config.json          # 公共配置与 active_config 入口
│   ├── config_v4l2.json     # V4L2 摄像头配置
│   └── config_4k_18mbps.json # 4K pull_stream 配置
├── src/
│   ├── main.cpp
│   ├── common/
│   │   ├── BlockingQueue.hpp    # 有界阻塞队列
│   │   └── Frame.hpp            # 帧数据结构（含 pixel_format 字段）
│   ├── capture/
│   │   ├── IVideoSource.hpp     # 视频源抽象接口
│   │   ├── CaptureThread.cpp    # V4L2 采集线程
│   │   └── PullStreamThread.cpp # RTMP/RTSP 拉流线程
│   ├── infer/
│   │   ├── InferThread.cpp      # RKNN 推理线程
│   │   ├── RockchipYoloPostprocess.cpp # 官方九输出后处理
│   │   ├── YoloPostprocess.cpp  # 旧双输出后处理 + 类别过滤
│   │   ├── ThumbnailTask.cpp    # 缩略图生成任务
│   │   ├── RoiFilter.cpp        # ROI 过滤与追踪
│   │   └── TilingTask.cpp       # ROI Tiling 推理
│   ├── encode/
│   │   ├── EncodeThread.cpp     # MPP 硬编线程
│   │   ├── font8x16.h           # ASCII 标签字体
│   │   └── font16x16.h          # 中文标签字体
│   ├── stream/
│   │   └── StreamThread.cpp     # RTMP 推流线程
│   └── mqtt/
│       └── MqttThread.cpp       # MQTT 上报线程
├── model/
│   └── .gitkeep                 # RKNN 模型文件不纳入 Git
└── third_party/
    ├── rknn/                    # RKNN Lite2 头文件
    ├── mpp/                     # MPP 头文件
    ├── rga/                     # RGA 头文件和链接库
    ├── paho/                    # Paho MQTT 头文件和链接库
    └── nlohmann/                # JSON 库
```

---

## 配置文件

`config/config.json` 保存公共配置，并只保留当前要使用的分配置文件名：

```json
{
  "active_config": "config_4k_18mbps.json",
  "model": { "path": "model/yolov8s_rockchip_dfl.rknn", "output_layout": "rockchip_dfl", "conf_threshold": 0.25, "iou_threshold": 0.45 },
  "detection": { "target_classes": ["cell phone", "cup", "keyboard", "mouse", "laptop", "book"], "report_interval_sec": 1 },
  "stream": { "draw_detection_labels": true },
  "mqtt": { "broker": "192.168.0.168", "port": 1883, "topic": "desk/detect", "client_id": "AlertGateway-01" }
}
```

两个分配置文件分别保存对应运行模式的完整差异配置：

- `config/config_v4l2.json`：V4L2 源、640×480、3 Mbps、对应 RTMP 地址，以及 Thumbnail/ROI/Tiling 开关。
- `config/config_4k_18mbps.json`：4K `pull_stream` 源、3840×2160、18 Mbps、对应 RTMP 地址，以及 4K 专用的 Thumbnail/ROI/Tiling 开关。

切换 4K 时只将 `active_config` 改为 `"config_4k_18mbps.json"`。程序会先加载公共配置，
再递归合并选中的分配置；视频源、分辨率、码率和图像处理参数均由对应模式配置明确维护。

> `source.type` 可设为 `"v4l2"`（本地摄像头，默认）或 `"pull_stream"`（RTMP/RTSP 拉流）。  
> 旧版 `camera` 节点继续支持，自动映射为 `source.type=v4l2`，向下兼容。

视频源配置示例：

- V4L2 本地摄像头：将 `active_config` 改为 `config_v4l2.json` 后使用
  `config/config.json`，或参考 `runs/testsrc2/config_v4l2_720p.json`，配置 `source.type`、
  `source.device`、分辨率和帧率。
- RTMP/RTSP 拉流：将 `source.type` 改为 `"pull_stream"`，并填写 `source.url`。

V4L2 的采集分辨率必须是摄像头实际支持的分辨率；当前编码输出尺寸跟随视频源尺寸，
不会仅通过配置把 V4L2 画面放大为 4K。4K 原始输入目前使用 `pull_stream` 配置。

当前工作区的 `config/config.json` 已选择 `config_4k_18mbps.json`，用于 201 板 4K 测试。
先将配置部署到 201 板：

```bash
scp config/config.json config/config_4k_18mbps.json \
  firefly@192.168.0.201:~/AlertGateway/config/
```

然后在 WSL/主机循环发布输入文件到 SRS；视频不是直接写入板端，201 板会从该 RTMP 地址拉流：

```bash
ffmpeg -re -stream_loop -1 \
  -i runs/input_videos/4k/VID_20260712_131410.mp4 \
  -map 0:v:0 -c copy -f flv \
  rtmp://192.168.0.168/live/testsrc2
```

在 201 板启动处理程序：

```bash
ssh firefly@192.168.0.201 \
  'cd ~/AlertGateway && ./AlertGateway config/config.json'
```

201 板拉取 `live/testsrc2` 输入流，完成 RKNN 推理和 MPP H.264 编码后输出到
`rtmp://192.168.0.168/live/alertgateway`，播放器可直接查看该地址。项目所有推理结果
推流统一使用该固定地址。输出流名称保留
历史上的 `v4l2_720p` 名称，实际尺寸为 3840×2160。恢复 V4L2 测试时，将
`active_config` 改回 `config_v4l2.json` 并重启程序。

---

## 编译

### 依赖

板子上：
- librknnrt.so（预装）
- librockchip_mpp.so（预装）
- librga.so（预装）
- libpaho-mqtt3as.so / libpaho-mqttpp3.so

WSL2 交叉编译工具链：
- aarch64-linux-gnu-g++ 11.4.0
- 板端 sysroot，默认路径为 `/home/rambos/sysroot`，需包含板端 FFmpeg-rockchip 6.1 头文件和 arm64 库

交叉编译时链接用的 `.so`（`third_party/rknn/librknnrt.so`、`third_party/rga/lib/librga.so.2.1.0`、
`third_party/paho/libpaho-mqtt3as.so.1`、`libpaho-mqttpp3.so.1`）属于板子厂商的预编译二进制，未随仓库提交（见 `.gitignore`）。
首次 clone 后需要从板子上把对应版本的 `.so` 拷到 `third_party/` 同名路径下，并准备实际运行用的 `model/yolov8s_rockchip_dfl.rknn`，再执行编译和部署。

### 编译命令

```bash
# WSL2 上
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
cmake --build build -j$(nproc)

# 传到 201 板
scp build/AlertGateway firefly@192.168.0.201:~/AlertGateway/
scp config/config.json config/config_4k_18mbps.json firefly@192.168.0.201:~/AlertGateway/config/
```

如果 sysroot 或 RKNN 链接 stub 不在默认路径，可显式传入：

```bash
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake \
  -DALERTGATEWAY_SYSROOT=/path/to/sysroot \
  -DRKNN_LIB_PATH=/path/to/librknnrt.so
```

---

## 运行

```bash
# 板子上
cd ~/AlertGateway

# 唯一启动入口；具体模式由 config/config.json 的 active_config 决定
./AlertGateway config/config.json
```

启动日志会打印当前加载的分配置路径。配置只在启动时读取；修改分辨率、帧率或码率后需要重启程序，程序仍兼容直接传入完整运行 JSON 文件。

---

## 配套系统

| 组件 | 平台 | 作用 |
|------|------|------|
| SRS | Windows | RTMP 流媒体服务器 |
| RambosPlayer | Windows | 拉流播放（含检测框） |
| MqttMonitor | Windows | 检测结果实时显示 |

---

## 文档说明

`docs/` 目录下的补充文档：

| 文档 | 内容 |
|------|------|
| `RGA预处理加速方案.md` | 用 RGA 硬件加速替换 InferThread 手写 CPU 预处理（YUYV→RGB888+缩放）的方案与验证结果 |
| `检测框消失问题排查记录.md` | 排查"输出视频检测框始终为空"问题的完整过程（最终定位为 INT8 量化时 box/class 共享 scale 导致分类精度丢失，见 BUG-009） |
| `NPU官方demo测速对比记录.md` | 用瑞芯微官方 rknn_model_zoo demo 跑同一模型交叉验证 NPU 推理耗时，排查 CPU governor 降频对测速的干扰 |
| `YOLOv5s与YOLOv8s-NPU耗时对比测试记录.md` | YOLOv5s vs YOLOv8s 的 `rknn_run` 对比测试，纠正"40ms 是硬件极限"结论，定位 CPU/NPU governor 对耗时的实际影响 |
| `YOLOv8s-RK3588推理性能优化路线.md` | YOLOv8s 在 RK3588 上的性能优化路线、阶段结论和后续方向 |
| `YOLOv8s-RK3588推理优化实验记录.md` | EXP-001 起的推理性能、精度、画质和输入尺寸实验记录 |
| `知乎文章-NPU推理耗时踩坑记录.md` | 上述测试过程的踩坑实录整理版 |
| `第三阶段-设备树入门.md` | RK3588S 设备树（Device Tree）入门记录 |
| `第五阶段-MPP硬解.md` | MPP 硬件解码开发记录 |
| `第五阶段-多媒体硬解完成记录.md` | 多媒体硬解功能完成记录 |
| `第六阶段-YOLOv8模型转换为RKNN格式.md` | YOLOv8 模型转换为 RKNN 格式的记录 |
| `第六阶段-YOLOv8 NPU推理(板子端).md` | YOLOv8 NPU 推理在板子端的部署记录 |
| `第六阶段-YOLOv8摄像头实时推理.md` | YOLOv8 摄像头实时推理功能记录 |
| `HDMI-Sunshine自动切换方案.md` | Firefly RK3588S 上 HDMI 输出与 Sunshine 远程游玩自动切换方案 |
| `WebRTC远程桌面方案.md` | RK3588S WebRTC 远程桌面方案 |

历史 bug 记录见根目录 `BUGS.md`；尚未落地、待评估收益的性能优化点见根目录 `TODO.md`。

---

## 开发计划

- [x] 项目骨架搭建（CMake + 目录结构）
- [x] 有界阻塞队列实现（`BlockingQueue`）
- [x] V4L2 采集线程（`CaptureThread`，mmap 零拷贝，双队列分发）
- [x] RKNN 推理线程（`InferThread`，RGA 预处理 + `decoded`/`rockchip_dfl` 双布局支持）
- [x] YOLOv8 后处理（`YoloPostprocess` / `RockchipYoloPostprocess`，DFL + NMS + 目标类别过滤）
- [x] 检测结果处理（画框逻辑并入 `EncodeThread`，汇总上报并入 `InferThread`）
- [x] MPP 编码线程（`EncodeThread`，YUYV→NV12 直转 + NV12 平面画框）
- [x] RTMP 推流线程（`StreamThread`，支持断线自动重连）
- [x] MQTT 上报线程（`MqttThread`）
- [x] 配置文件加载（nlohmann/json）
- [x] RGA 硬件加速预处理（已实施并验证）
- [x] 叠框中文标签与可配置码率（已实施并验证）
- [x] 后处理循环 L1/L2 缓存优化（中位数 4.14 ms → 2.86 ms，降幅 31%）
- [x] 持久化 performance 锁频服务（`tools/performance/rockchip-performance.service`）
- [x] **视频源抽象层**（`IVideoSource` + `PullStreamThread`，支持 V4L2 与 RTMP/RTSP 拉流）
- [x] **V4L2/4K 配置选择**（`active_config`、V4L2 配置与 4K `pull_stream` 配置）
- [x] **Thumbnail 缩略图任务**（NV12 缩略图、MQTT 附图；RGA/JPEG 仍为后续优化）
- [ ] **ROI 区域过滤与追踪**（代码已实现；归一化坐标、帧间 IoU 追踪、停留事件待板端专项验证）
- [ ] **ROI Tiling 推理**（代码已实现；小目标增强、坐标映射、全局 NMS 合并待板端专项验证）
- [ ] 多路架构扩展（Channel ID 抽象，多路 `PullStreamThread` 实例）

---

## License

本项目代码以 MIT License 开源，见 `LICENSE`。`third_party/` 下的头文件（RKNN、MPP、RGA、Paho MQTT、nlohmann/json）保留各自上游的原始授权条款。



## 2026-07-14 4K pull-stream output and bitrate validation

The 4K pull-stream path now forwards decoded source frames to the encoder without the previous pre-encode 30-to-15 FPS pacing. Encoder and stream queues use non-blocking pushes to prevent backpressure from building latency; inference continues in latest-frame mode and may drop stale inference frames.

For 4K testing, stream.bitrate_kbps is configurable with presets: 6000, 12000, and 18000. Preset files are under runs/testsrc2/: config_testsrc2_4k_6m.json, config_testsrc2_4k_12m.json, and config_testsrc2_4k_18m.json. The current workspace entry config selects config_4k_18mbps.json; restore the V4L2 configuration by selecting config_v4l2.json.

Corrected 30 FPS 12/18 Mbps board validation: 12 Mbps measured 12007.9 kbps at 29.99 FPS; 18 Mbps measured 18087.0 kbps at 30.08 FPS. Both had zero write failures and zero queue depth. In the aligned single-frame quality sample, PSNR was 28.4200/28.4099 dB and SSIM was 0.5735/0.5813 for 12/18 Mbps respectively. Full captures and logs are in runs/testsrc2/quality_compare_12v18_20260714/.

Detailed test record: docs/4k/4k_30fps_bitrate_validation_20260714.md

4K document index: docs/4k/README.md
