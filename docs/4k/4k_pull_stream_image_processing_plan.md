# AlertGateway 单路 4K 架构设计

> 阶段：**架构设计**（已确认）  
> 后续开发以本文档为准，不超出本文档描述的范围。

---

## 核心设计决策

| 决策项 | 结论 |
|---|---|
| 视频源方式 | V4L2 本地采集 与 RTMP/RTSP 拉流 均为一等可配置项，通过 `source.type` 切换 |
| ROI 坐标系 | 归一化浮点 0.0～1.0，与输入分辨率解耦 |
| Tiling 范围 | 仅对命中 ROI 的区域做 Tiling，无 ROI 命中回退整图缩放推理 |
| 推理次数上限 | 单帧控制在 1～2 次，兼顾小目标检测与整体帧率 |
| 现有代码边界 | `CaptureThread` / `InferThread` / `EncodeThread` 核心逻辑保持不变 |

---

## 一、视频源抽象层

### 新增模块

| 模块 | 说明 |
|---|---|
| `IVideoSource` | 抽象接口，定义 `start()` / `stop()` 两个方法 |
| `PullStreamThread` | 实现 `IVideoSource`，通过 FFmpeg 拉取 RTMP/RTSP 流并解码 |

### 关键改动点

- `CaptureThread` 补充实现 `IVideoSource`（仅新增继承关系，业务不变）
- `Frame` 新增 `pixel_format` 字段，区分 YUYV（V4L2 来源）和 NV12（拉流来源）
- `main.cpp` 根据 `source.type` 实例化对应视频源，其余模块无感知
- `InferThread` 和 `EncodeThread` 根据 `frame.pixel_format` 选择已有的对应预处理路径

### Config 新增节点

```
source.type         = "v4l2" | "pull_stream"
source.device       = 设备节点（v4l2 时有效）
source.url          = 流地址（pull_stream 时有效）
source.width/height = 分辨率
source.fps          = 帧率
source.reconnect_sec = 断线重连间隔（pull_stream 时有效）
```

原有 `camera` 节点继续兼容，作为 `source.type=v4l2` 的别名。

---

## 二、图像处理层（三类可配置任务）

### 设计原则

- 三类任务全部默认关闭，不影响现有链路
- 统一挂载在 `InferThread` 推理流程中
- 任务间可独立启用，ROI + Tiling 可联动（Tiling 仅作用于 ROI 区域）

### 任务 A：Thumbnail 缩略图

**位置**：InferThread 检测完成后  
**作用**：生成指定尺寸的缩略图，附加到 MQTT 上报 payload  
**关键点**：
- 使用 RGA 硬件缩放，CPU 兜底
- 可配置仅在检测到目标时生成（`on_detection_only`）

**Config**：
```
image_processing.thumbnail.enabled
image_processing.thumbnail.width / height
image_processing.thumbnail.on_detection_only
```

---

### 任务 B：ROI 区域过滤与追踪

**位置**：InferThread 推理调度处  
**作用**：圈定感兴趣区域，推理只处理 ROI 内容，结果坐标映射回全图  
**关键点**：
- ROI crop → RGA 缩放到 640×640 → 推理 → 坐标反算回全图
- `filter_outside=true` 时，ROI 外的检测结果不上报
- 追踪（`track_dwell_sec > 0`）：帧间 IoU 匹配，统计目标在 ROI 内停留时长，超阈值触发事件上报

**Config**：
```
image_processing.roi.enabled
image_processing.roi.regions[]      = [{id, x, y, w, h}]（归一化 0.0～1.0）
image_processing.roi.filter_outside
image_processing.roi.track_dwell_sec
```

---

### 任务 C：ROI Tiling 分割推理

**位置**：ROI 命中后，对 ROI 区域内部做网格切分  
**作用**：对 ROI 内区域切为若干 Tile 分别推理，弥补 4K 缩放后小目标漏检  
**关键点**：
- 仅对命中 ROI 的区域切图，不做全图 Tiling
- 单帧最多推理 1～2 次（推荐 1×2 或 2×1 切分）
- 各 Tile 检测框坐标映射回全图后，做全局 NMS 去重（复用现有 NMS 逻辑）
- 无 ROI 命中时，回退整图缩放推理

**Config**：
```
image_processing.tiling.enabled
image_processing.tiling.grid_cols / grid_rows
image_processing.tiling.overlap_ratio
image_processing.tiling.merge_iou_threshold
```

---

## 三、多路架构预留

此阶段**仅预留扩展点，不实现多路业务**：

- `PullStreamThread` 构造函数预留 `channel_id` 参数
- `Frame` 预留 `channel_id` 字段
- `SharedDetections` 预留按 Channel 分桶的接口
- Config 预留 `channels[]` 数组节点（当前只解析 index 0）

---

## 四、新增文件清单

| 文件 | 类型 | 说明 |
|---|---|---|
| `src/capture/IVideoSource.hpp` | 新增 | 视频源抽象接口 |
| `src/capture/PullStreamThread.hpp/cpp` | 新增 | RTMP/RTSP 拉流线程 |
| `src/infer/ThumbnailTask.hpp/cpp` | 新增 | 缩略图生成任务 |
| `src/infer/RoiFilter.hpp/cpp` | 新增 | ROI 过滤与追踪 |
| `src/infer/TilingTask.hpp/cpp` | 新增 | ROI Tiling 推理 |

## 五、需要改动的现有文件

| 文件 | 改动性质 |
|---|---|
| `src/capture/CaptureThread.hpp` | 新增 `IVideoSource` 继承（最小改动） |
| `src/common/Frame.hpp` | 新增 `pixel_format` 字段和枚举 |
| `src/main.cpp` | 改配置解析 + 视频源工厂（`source.type` 分支） |
| `src/infer/InferThread.cpp` | 新增格式判断 + 三类任务调用入口 |
| `src/encode/EncodeThread.cpp` | 新增 NV12 直通路径（跳过 YUYV→NV12 转换） |
| `config/config.json` | 新增 `source` 和 `image_processing` 节点 |
| `CMakeLists.txt` | 新增源文件注册 |

---

## 六、验收标准（按阶段）

### 阶段一：视频源切换
- `source.type=v4l2` 行为与现有完全一致
- `source.type=pull_stream` 稳定拉流 60 秒无错误，帧正确进入推理与编码链路

### 阶段二：三类图像处理
- 三类任务均 `enabled=false` 时，与阶段一行为一致（零影响）
- Thumbnail：MQTT payload 含缩略图，尺寸正确
- ROI：仅 ROI 内目标上报，全图坐标映射正确
- Tiling：ROI 内切图推理，全局 NMS 后无重复框
