# AlertGateway

基于 RK3588S 的桌面物品检测系统（YOLO 检测流程验证 Demo）。

边缘端 C++ 应用，运行在 Firefly ROC-RK3588S-PC 开发板上，摄像头俯拍桌面，实现摄像头采集、NPU 推理、检测结果画框、硬件编码推流、MQTT 上报的完整业务闭环，用于验证手机/杯子/键盘等桌面物品的 YOLO 检测全链路。

经过多轮性能调优，单帧 NPU 推理（`rknn_run`）耗时在 ~31-45ms 区间，**实测发现这个耗时主要受 CPU 主频影响而非纯 NPU 硬件算力瓶颈**：CPU+NPU governor 都锁 `performance` 时可压到 ~31ms，真实运行时（CPU 频率随系统负载动态变化）在 32-51ms 间波动，详见 `docs/YOLOv5s与YOLOv8s-NPU耗时对比测试记录.md`。编码推流稳定运行在摄像头实际帧率，RTMP 端到端延迟 < 1s，支持断线自动重连。

---

## 系统架构

```
┌─────────────────────────────────────────────────────┐
│                   AlertGateway                       │
│                                                      │
│  采集线程                                            │
│  V4L2 ──┬──────────────→  编码线程  →  推流线程       │
│  /dev/video             MPP硬编      RTMP推流        │
│         │                 h264_rkmpp                │
│         │                    ▲                      │
│         │            叠加最新检测框(SharedDetections) │
│         └──→  推理线程  ───────┘                     │
│              RKNN NPU                                │
│              YOLOv8s  ──→  MQTT线程                  │
│                            检测结果上报               │
│                            Paho MQTT                 │
└────────────────────┬────────────────────────────────┘
                     │
        ┌────────────┴────────────┐
        ▼                         ▼
  SRS（Windows）            MqttMonitor（Windows）
  RTMP接收                   检测结果实时显示
        │
        ▼
  RambosPlayer（Windows）
  拉流播放（含检测框画面）
```

---

## 功能说明

### 核心业务
- 摄像头俯拍桌面，实时采集画面
- YOLOv8s NPU 推理，检测桌面上的目标物品（手机、杯子、键盘等）
- 检测结果叠加画框（类别 + 置信度）后硬编码推流到 RTMP 服务器，便于直观验证检测效果
- 检测结果（物品种类、数量、置信度、坐标）通过 MQTT 周期上报到上位机

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
采集、推理、编码三条线程通过独立队列解耦：采集线程同时向编码队列（阻塞、不丢帧）和推理队列（非阻塞、可丢帧，始终推理最新帧）推送数据，推理速度不再决定编码推流帧率，编码线程稳定运行在摄像头采集帧率上。推理结果通过线程安全的共享结构传递给编码线程叠加画框，两条线程互不阻塞。

### NPU 推理性能
- **INT8 量化 + 双输出张量**：模型导出时将检测框回归（数值范围 0-640）与分类置信度（数值范围 0-1）拆分为两个独立输出张量分别量化校准，避免共享同一套量化参数导致分类精度损失，检测结果更可靠
- **Zero-copy 输入 + RGA 硬件预处理**：摄像头原始 YUYV 画面通过 RGA 硬件加速器一次性完成颜色空间转换与缩放，直接写入 NPU 的 DMA 输入缓冲区，省去 CPU 软件转换开销，预处理耗时降至 1ms 级
- 单帧 NPU 推理（`rknn_run`）耗时 ~31-45ms，**不是固定的硬件极限**：实测发现该耗时里包含受 CPU 主频影响的同步开销，CPU idle 408MHz 时高达 59ms，CPU+NPU governor 都锁 `performance` 时可压到 ~31ms；真实运行时因 governor 只按需顶部分核心，耗时在 32-51ms 间波动。完整测试过程见 `docs/YOLOv5s与YOLOv8s-NPU耗时对比测试记录.md`

### 推流稳定性
- RTMP 推流参数（GOP、编码 profile、队列深度）针对低延迟场景调优，端到端延迟控制在 1 秒以内
- 推流连接具备自动断线重连能力，网络抖动或服务端重启后无需人工介入即可恢复推流

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
| 摄像头采集 | V4L2 |
| AI 推理 | RKNN Lite2 C API + YOLOv8s |
| 硬件编码 | Rockchip MPP（h264_rkmpp） |
| 推流 | RTMP |
| 消息上报 | Paho MQTT C++ |
| 线程通信 | 有界阻塞队列 + 条件变量 |
| 配置管理 | JSON（nlohmann/json） |

---

## 目录结构

```
AlertGateway/
├── CMakeLists.txt
├── README.md
├── config/
│   └── config.json          # 运行时配置
├── src/
│   ├── main.cpp
│   ├── common/
│   │   ├── BlockingQueue.hpp    # 有界阻塞队列
│   │   └── Frame.hpp            # 帧数据结构
│   ├── capture/
│   │   └── CaptureThread.cpp    # V4L2 采集线程
│   ├── infer/
│   │   ├── InferThread.cpp      # RKNN 推理线程
│   │   └── YoloPostprocess.cpp  # 后处理 + 类别过滤
│   ├── encode/
│   │   └── EncodeThread.cpp     # MPP 硬编线程
│   ├── stream/
│   │   └── StreamThread.cpp     # RTMP 推流线程
│   └── mqtt/
│       └── MqttThread.cpp       # MQTT 上报线程
├── model/
│   └── yolov8s.rknn             # RKNN 模型文件
└── third_party/
    ├── rknn/                    # RKNN Lite2 头文件
    ├── mpp/                     # MPP 头文件
    └── nlohmann/                # JSON 库
```

---

## 配置文件

`config/config.json`：

```json
{
  "camera": {
    "device": "/dev/video20",
    "width": 640,
    "height": 480,
    "fps": 30
  },
  "model": {
    "path": "model/yolov8s.rknn",
    "conf_threshold": 0.25,
    "iou_threshold": 0.45
  },
  "detection": {
    "target_classes": ["cell phone", "cup", "keyboard", "mouse", "laptop", "book"],
    "report_interval_sec": 1
  },
  "stream": {
    "rtmp_url": "rtmp://<YOUR_SRS_SERVER_IP>/live/desk"
  },
  "mqtt": {
    "broker": "<YOUR_MQTT_BROKER_IP>",
    "port": 1883,
    "topic": "desk/detect",
    "client_id": "AlertGateway-01"
  }
}
```

---

## 编译

### 依赖

板子上：
- librknnrt.so（预装）
- librockchip_mpp.so（预装）
- libpaho-mqtt3c

WSL2 交叉编译工具链：
- aarch64-linux-gnu-g++ 11.4.0

交叉编译时链接用的 `.so`（`third_party/rknn/librknnrt.so`、`third_party/rga/lib/librga.so.2.1.0`、
`third_party/paho/libpaho-mqtt3as.so.1`、`libpaho-mqttpp3.so.1`）属于板子厂商的预编译二进制，未随仓库提交（见 `.gitignore`）。
首次 clone 后需要从板子上把对应版本的 `.so` 拷到 `third_party/` 同名路径下，再执行编译命令，否则链接会失败。

### 编译命令

```bash
# WSL2 上
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-toolchain.cmake
make -j$(nproc)

# 传到板子
scp AlertGateway firefly@192.168.0.200:~/AlertGateway/
```

---

## 运行

```bash
# 板子上
cd ~/AlertGateway
./AlertGateway config/config.json
```

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
- [x] RKNN 推理线程（`InferThread`，RGA 预处理 + 双输出张量分别量化）
- [x] YOLOv8 后处理（`YoloPostprocess`，NMS + 目标类别过滤）
- [x] 检测结果处理（画框逻辑已并入 `EncodeThread`，汇总上报已并入 `InferThread`；原独立的 `DetectionReporter` 已废弃并删除）
- [x] MPP 编码线程（`EncodeThread`，YUYV→NV12 直转 + NV12 平面画框）
- [x] RTMP 推流线程（`StreamThread`，支持断线自动重连）
- [x] MQTT 上报线程（`MqttThread`）
- [x] 配置文件加载（nlohmann/json）
- [ ] 多路扩展支持
- [x] RGA 硬件加速预处理（已实施并验证，见 `docs/RGA预处理加速方案.md`）

---

## License

本项目代码以 MIT License 开源，见 `LICENSE`。`third_party/` 下的头文件（RKNN、MPP、RGA、Paho MQTT、nlohmann/json）保留各自上游的原始授权条款。
