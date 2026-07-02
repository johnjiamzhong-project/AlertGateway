# AlertGateway 桌面物品检测系统 — 技术总结

## 项目概述

基于 RK3588S（Firefly ROC-RK3588S-PC）+ YOLOv8s NPU 实现桌面物品实时检测系统。摄像头俯拍桌面，实时识别手机、杯子、键盘等常见物品，检测结果叠加画框后通过 MPP 硬编码推流至 RTMP 服务器，同时通过 MQTT 上报物品种类与数量变化。

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

## 事故链：整个项目是被 bug 推着走的

这份文档不讲"最终长什么样"，按**时间线**讲每个主要问题是怎么发现、排查、修复的，以及每个修复又暴露了什么新问题。

项目的所有关键设计决策（并行流水线、SharedDetections、INT8 量化、断线重连）**都不是预先设计出来的**，都是被具体的事故逼出来的。每个事故链都按"当时状态 → 出了什么问题 → 排查路径 → 改了什么 → 最终效果"来组织，末尾附面试常追问题。

完整 bug 记录见 `BUGS.md`，单话题的面试笔记见 `notes/` 目录。

---

### 事故链一：Pipeline 串行 → 并行（BUG-005）

#### 当时的状态

最初的 pipeline 是串行的：

```
CaptureThread → [queue] → InferThread → [queue] → EncodeThread → StreamThread
```

一帧的完整路径：采集 → YUYV→RGB 全图转换（640×480×3）→ RKNN NPU 推理 → RGB→NV12 再转换 → MPP 编码 → RTMP 推流。所有步骤串在一根线上。

#### 出了什么问题

RTMP 推流画面严重卡顿，实测 **~5fps**，远低于摄像头 14.6fps 的能力。NPU 推理一帧总耗时约 200ms（FP16 模型时期），整条 pipeline 的吞吐被 NPU 锁死。摄像头采集的帧在 InferThread 前排长队，编码线程无事可做。

附加问题：`EncodeThread` 做了两遍颜色转换（YUYV→RGB 给推理用，RGB→NV12 给编码用），RGB 中间缓冲纯粹是浪费。8

#### 排查路径

在 InferThread 里逐段加计时，确认各阶段耗时分布后锁定 NPU 是瓶颈。分析串行拓扑发现 InferThread 阻塞时 EncodeThread 在空等，两者应该解耦。

#### 改了什么

**1. Pipeline 解耦为并行双路**

```
CaptureThread ──→ [enc_queue=1, 阻塞]   → EncodeThread → StreamThread
              └──→ [infer_queue=2, 非阻塞] → InferThread
                                                   ↓ SharedDetections
                                              EncodeThread 读
```

- `enc_queue` 容量 1，阻塞推送（不丢帧），是整条流水线唯一的背压点
- `infer_queue` 容量 2，非阻塞推送（InferThread 忙时直接丢帧）
- InferThread 每次取帧时排空队列，始终推理"最新一帧"，不排队

**2. SharedDetections 共享区**

```cpp
struct SharedDetections {
    mutable std::mutex mutex;
    std::vector<Detection> detections;
    void set(std::vector<Detection> dets);    // InferThread 写，覆盖上次结果
    std::vector<Detection> get() const;       // EncodeThread 读，返回副本
};
```

为什么不用 BlockingQueue 传检测结果？因为编码线程每帧都要叠框，如果用队列，队列空时编码线程就得等——等于又把解耦的设计推回去了。SharedDetections 只保留"最新一份"，编码线程拿到什么用什么，永不等待。代价是画的框可能是上一次推理的，但 ~48ms 推理 vs ~67ms 出帧间隔下无感知。

**3. YUYV→NV12 直转消除 RGB 中间格式**

推理有独立的 RGA 预处理路径（YUYV→RGB+缩放），编码这边直接手写 YUYV→NV12 的纯 CPU 转换，两者不再共享中间 RGB buffer。

#### 最终效果

| 指标 | 优化前 | 优化后 |
|------|-------|-------|
| 推流帧率 | ~5fps（被 NPU 拖死） | 14.6fps（摄像头极限）|
| 编码与推理关系 | 串行，推理拖编码 | 并行，互不等待 |
| 颜色转换 | YUYV→RGB→NV12 两次 | 编码侧 YUYV→NV12 直转，推理侧 RGA 硬件转 |

#### 面试追问

**Q：为什么 enc_queue 只设容量 1？**
因为容量 1 意味着 CaptureThread 投递下一帧时，如果 Encoder 还没消费完上一帧，push(100ms timeout) 会超时失败。这是有意设计的背压点：编码跟不上时主动卡一下采集，而不是让队列无限堆积增加延迟。100ms 超时避免采集线程永久卡死。

**Q：InferThread 为什么非阻塞投递 + 排空旧帧？**
检测系统要的是实时性，不是完整率。处理器材画面出现的新物体，用最新帧推理最重要，排队等着推理 10 帧前的画面毫无意义。非阻塞投递 + 排空旧帧两层保证 NPU 永远在处理当下最新画面。

**Q：SharedDetections 如果推理比编码慢很多会怎样？**
推理 ~48ms 一帧，摄像头 ~67ms 一帧，两者量级接近，这种情况下一帧滞后编码线程读到的基本是上一帧的检测框。如果推理慢到 200ms（FP16 时期），编码线程读到的是 3 帧前的框，15fps 下约 200ms 的视觉滞后，桌面检测场景依然可接受。但如果帧率差到 10 倍以上（比如推理 500ms、编码 30fps），滞后就明显了，那时候需要重新评估设计。

**Q：为什么不做新鲜度判断，超时就不画框？**
可以做，但要确保实现方式是一次性检查，不能变成等待循环。"查不到继续等"等于让编码线程重新等推理线程，跟解耦的目标矛盾，且会在 NPU 真正出故障时触发——专挑故障时刻发作。正确做法：`get()` 同时返回时间戳，超过阈值就跳过这一帧的画框，不等不重试，正常往下编码。

#### 补充：推理降频机制（infer_every_n_frames）

config.json 中 `model.infer_every_n_frames` 控制推理频率，默认值 1 表示每帧都推理。当 NPU 推理速度（~48ms/帧）接近或超过摄像头帧间隔（~67ms/帧）时，可以设为 2 或更大值让推理线程跳帧，进一步降低 NPU 负载。

```cpp
// InferThread::run() 中的跳帧逻辑
const int step = std::max(1, cfg_.infer_every_n_frames);
// ...
if (frame_idx % step == 0) {
    // 执行 NPU 推理
}
frame_idx++;
```

**为什么不用固定抽帧而是用模运算？** 模运算保证推理帧均匀分布，不会出现"连续推理 N 帧然后连续跳 N 帧"的抖动。跳过的帧仍然走 MQTT 上报逻辑（用上一次的 `last_detections`），不会中断检测结果的周期性推送。

当前配置 `infer_every_n_frames=1`（每帧推理），因为 INT8 优化后 NPU 已跑满摄像头帧率，不需要降频。这个参数是留给未来分辨率/模型升级时的调节旋钮。

---

### 事故链二：FP16 → INT8 量化 → 量化精度丢失（BUG-007 → BUG-009）

#### 第一阶段：发现 FP16 性能只有预期一半（BUG-007）

**当时状态**：串行 pipeline 已解耦，但 FP16 模型推理总耗时 ~150ms，摄像头 14.6fps（~67ms/帧），推理还是跑不满帧率。

**排查**：加计时代码分段打点，各阶段耗时：
```
cpu（YUYV→RGB+resize）:  8ms
rknn_inputs_set:         37ms
rknn_run（NPU 计算）:    75ms
rknn_outputs_get:        37ms
total:                  150ms
```

`rknn_run` 75ms 比 YOLOv8s INT8 在 RK3588S 上的参考值（30-40ms）高一倍。怀疑模型没做 INT8 量化，用 `strings` 直接检查 rknn 文件：

```bash
strings yolov8s.rknn | grep -E 'dtype|quant|int8|float'
# 输出显示 "dtype": "float16"，量化参数段为空
```

**结论：模型是 FP16，从未做过 INT8 量化。** NPU 以 16 位浮点运算，天然比 INT8 慢一倍。

**修复**：采集 150 张真实场景校准图，在 WSL 上用 rknn-toolkit2 重新量化。转换过程踩到两个坑：
- onnx 版本冲突：系统 onnx 1.22.0 移除了 `onnx.mapping`，rknn-toolkit2 依赖此接口。降级到 1.13.1。
- `rknn.build(dataset=...)` 参数类型必须是文件路径字符串，不能传 numpy 数组。

| 阶段 | FP16（前） | INT8（后） |
|------|-----------|-----------|
| rknn_run | 75ms | **38ms** |
| inputs_set | 37ms | **0.4ms** |
| outputs_get | 37ms | **1-3ms** |
| **total** | **~150ms** | **~48ms** |

推理能力从 6.7 FPS 提升至 ~20 FPS，跑满摄像头帧率。

#### 第二阶段：INT8 后检测框全空（BUG-009）

**现象**：换 INT8 模型后，输出视频里一个检测框都没有，调低 `conf_threshold` 也无济于事。

**根因（两层）**：

1. **主因：box/cls 共享量化 scale 把分类精度冲掉了**
   YOLOv8s 在 ONNX 导出时，box 坐标（4 通道，值范围 0-640）和分类概率（80 通道，值范围 0-1）是合并成一个 84 通道输出张量的。INT8 量化时共享一组 scale（约 2.6），这个 scale 按 box 坐标范围校准，class 概率在整个 0-1 区间不到 1 个 INT8 量化档位，反量化后全部精确归零。分类信息在量化这一步就丢了，运行时代码无法补救。

2. **次因：后处理重复做 sigmoid**
   YOLOv8 导出的 ONNX 图分类分支自带 Sigmoid 节点，但 C++ 后处理又手动做了一次 sigmoid，重复计算。

   **修复**：
- ONNX 导出时用 `outputs` 参数在最后一次 concat **之前**把 box 和 class 拆成两个独立输出张量，各自拥有独立的量化 scale
- C++ 侧用 `rknn_outputs_get(want_float=1)` 分别取 box/cls 浮点值（由 driver 做反量化）
- 后处理 `YoloPostprocess::process()` 改为同时接收 box/cls 两个缓冲区
- 去掉重复的 sigmoid 调用

**加固**：代码里不假定 box/cls 的输出顺序固定，而是按元素个数自动识别（`4×8400` vs `80×8400`），防止模型导出顺序变化或调整输出数量时出问题。

#### 第三阶段：RGA 硬件预处理 + NPU zero-copy 输入

CPU 做 YUYV→RGB+缩放一次约 8-10ms，走 RGA 硬件加速器一次完成（~1ms），代码设计为 **RGA 优先 + CPU 兜底**：RGA 偶发失败时自动回退到 CPU 路径，保证推理不被一个硬件加速器的偶发失败卡死。

此外，INT8 量化后 `rknn_inputs_set` 从 37ms 降到 0.4ms，但代码进一步做了 **NPU DMA zero-copy 输入**：在 `load_model()` 时预分配 `rknn_create_mem()` 的 DMA 缓冲区并用 `rknn_set_io_mem()` 绑定为模型输入，后续每帧推理直接 `memcpy` 进 DMA 缓冲区，省掉了 `rknn_inputs_set` 的额外拷贝开销。INT8 后这个开销已经很小（0.4ms），但 zero-copy 设计让输入路径更简洁，也为未来 RGA 直接输出到 DMA 缓冲区（真正的硬件 zero-copy）预留了扩展点。

在测试中发现的额外问题：`rknn_run` 的耗时并不稳定，从 ~31ms（锁 performance governor）到 ~59ms（CPU idle 408MHz）波动。**NPU 依赖 CPU 配置推理任务、处理中断，CPU 主频低时 NPU 的吞吐也上不去。** 这是嵌入式 SoC 的典型耦合问题：看似加速器独立工作，实际上 CPU-NPU 共享总线，CPU 降频会导致 NPU 带宽受限。

**解决方案**：项目在 [tools/performance/](file:///home/rambos/arm_test/AlertGateway/tools/performance/) 下提供了自动化调优脚本与 systemd 服务，使用 [deploy.sh](file:///home/rambos/arm_test/AlertGateway/tools/performance/deploy.sh) 可一键在板端部署 `rockchip-performance` 开机自启服务，确保 CPU & NPU 全程锁定为 `performance` 模式，将推理时延稳定压在 **~31ms**（支持随时停止服务还原默认设置）。

这也是 SharedDetections 设计的额外收益：NPU 偶尔慢到 59ms 时，编码帧率不受影响。

#### 面试追问

**Q：拿到 rknn 模型后第一步做什么？**
`strings` 检查 `dtype` 字段确认量化类型。FP16 性能约 INT8 一半，这是最容易犯的错误——rknn 文件不显示量化类型，不检查的话可能一直在用低效模型而不自知。

**Q：INT8 量化校准图怎么选的？**
从真实部署场景采集 150 张图（桌面俯拍、不同光照、不同物品摆放），覆盖推理时会遇到的画面分布。校准图质量直接影响量化精度：图太少或跟场景不匹配，量化后各层激活值分布统计不准，精度会下降。

**Q：为什么 box/cls 要拆开量化？**
两个输出数值量级差几百倍：box 坐标 0-640，class 概率 0-1。共享一个 scale 时，scale 按大数值（box）校准，小数值（class）在这个 scale 下不到 1 个 INT8 档位，相当于整个 class 输出被量化为 0，这就是 BUG-009 检测框全空的根因。

**Q：RGA 为什么会偶发失败？**
嵌入式硬件加速器依赖驱动和硬件状态，在某些竞争条件（多个硬件模块争总线、驱动 bug、资源不足）下可能失败。设计时留了 CPU 兜底路径，不是为了优雅，是因为嵌入式场景下硬件加速不可靠是常态。

**Q：YOLOv8 的 NMS 为什么只在同一个类别内做抑制，不同类别的框可以重叠？**
NMS（非极大值抑制）的语义是"同一个目标产生了多个重叠候选框，只保留分数最高的那个"。不同类别的目标（如桌面上紧挨的杯子和键盘）天然可以重叠，相互之间不应抑制。若做全类别 NMS，两个位置相近的不同类别目标可能互相抑制掉对方，导致漏检。本项目 `YoloPostprocess::nms()` 里只对 `class_id` 相同的框做 IoU 判断，不同类别框允许重叠共存，与 YOLOv8 官方后处理行为一致。

**Q：NPU 输入为什么要用 `rknn_create_mem` 分配 DMA 缓冲区，而不是普通 `malloc`？**
`rknn_create_mem` 分配的是 NPU 驱动管理的连续物理内存（CMA 区域或 ION 分配器），CPU 和 NPU 都能直接访问，CPU 写入后 NPU 可通过 DMA 读取，无需内核额外拷贝。用普通堆内存时，`rknn_inputs_set` 必须把数据从用户态堆内存拷贝到 NPU 可见的物理内存——这正是 FP16 时期 `rknn_inputs_set` 耗时 37ms 的原因（每帧做一次 640×640×3 字节的大块拷贝）。Zero-copy 绑定后该拷贝消失，代价是 DMA 缓冲区必须在模型加载时提前分配并绑定，不能按需临时申请。MPP 侧的 DRM buffer group（`MPP_BUFFER_TYPE_DRM`）同理：硬件编码器能直接 DMA 访问这类物理连续内存，普通 `malloc` 堆内存无法直接被硬件使用。

---

### 事故链三：RTMP 推流与重连（BUG-008）

#### 当时的状态

StreamThread 用 FFmpeg 向 SRS 推流，连接建立后没有断线检测和重连逻辑。

```
EncodeThread → [stream_queue=2] → StreamThread → av_write_frame → SRS
```

#### 出了什么问题

SRS 重启后，推流永不复原，且编码帧率从 14.6fps 暴跌到 ~5fps。需手动 SSH 上板子重启 AlertGateway 才能恢复。

实际排查才发现这不是一个 bug，是**三个叠在一起的 bug**：

#### Bug 1：断线检测失效（主因）

```cpp
int ret = av_write_frame(fmt_ctx_, pkt);
if (ret >= 0) avio_flush(fmt_ctx_->pb);  // flush 返回值未检查
return ret >= 0;
```

`av_write_frame` 把数据写入 TCP 发送缓冲区返回成功，但 `avio_flush` 触发真正 TCP 发送时才拿到 EPIPE/ECONNRESET。flush 的返回值被静默忽略，`write_packet` 永远返回 true，断线永远检测不到。

**修复**：`avio_flush` 后检查 `fmt_ctx_->pb->error` 错误字段。

#### Bug 2：重连后没有 SPS/PPS

MPP 编码器只在**启动时第一个关键帧**里带 SPS/PPS。原 `close_rtmp()` 会清空 `sps_/pps_` 缓存，重连后等不到新的 SPS/PPS（编码器不会在运行中断后重发），`write_extradata` 永远不会被调用，`header_written_` 始终为 false，`write_packet` 提前 return true 什么也不发。SRS API 表现为 `active: false`，播放器拉不到流。

**修复**：重连时保留 `sps_/pps_` 缓存，连上后立刻用缓存值写 header。

#### Bug 3：重连线程退出导致背压传导

原代码重连一次失败即 `break` 退出线程——没人消费 stream_queue。背压逆流：stream_queue 满 → EncodeThread push 阻塞 → enc_queue 满 → CaptureThread push 超时 → 采集帧率暴跌。

**修复**：改为无限重试（每 3 秒一次），重试期间持续 `pop` 排空队列以防背压。

#### Bug 4：心跳检测——静默断线的兜底

三个 bug 修复后，还有一个边界场景：如果网络断开但 EncodeThread 暂时没推包（比如编码卡顿），`write_packet` 不会被调用，单靠"写失败才重连"会迟迟发现不了断线。

代码在 `run()` 主循环中加了心跳检测：队列 200ms 轮询连续 5 秒无成功写包时，主动检查 `fmt_ctx_->pb->error`，发现错误立即触发重连。

```cpp
// StreamThread::run() 心跳逻辑
if (!in_queue_.pop(ep, 200)) {
    if (header_written_ && clk::now() - last_ok > std::chrono::seconds(5)) {
        if (fmt_ctx_->pb->error < 0) {
            reconnect_loop();
        }
    }
    continue;
}
```

这是一种"被动检测 + 主动探测"的双保险策略：正常推流时靠 `write_packet` 返回值检测断线（被动），空闲时靠心跳定期探测连接状态（主动）。

#### 这三个 bug 的关联

各自独立，但在一次 SRS 重启中全部触发：断线检测不到（Bug1）→ 即使侥幸检测到了，重连也发不出数据（Bug2）→ 即使发不出数据，线程退出时还拖垮了编码帧率（Bug3）。修单个 bug 无法完全恢复推流。

#### 额外弯路：SRS gop_cache 排查

修复重连后发现另一个问题：RambosPlayer 出现周期性 ~550ms 的冻帧。日志显示 547ms 间隔收不到帧，然后一次收到 8 帧爆发。这是 SRS `gop_cache` 的典型行为：在 14.6fps、GOP=8 时，GOP 周期正好 547ms。

排查过程遇到两个弯路：

**弯路一：改错了配置文件**
SRS 命令行参数是 `-c conf\live.conf`，但 `Get-WmiObject` 确认实际进程加载的是 `conf\console.conf`。改了一堆配置文件都没生效。

**弯路二：HLS enabled 让 gop_cache off 无效**
找到正确配置后加上 `gop_cache off`，卡顿仍在。原因：`console.conf` 中有 `hls { enabled on; }`，HLS 切片必须从 I 帧开始，SRS 内部会为 HLS 维护 GOP 缓冲，这个缓冲与 RTMP play 路径耦合，导致 RTMP 订阅者收到的仍是 GOP 批量数据。

**最终修复**：去掉 HLS、RTC、http_remux 等块，用最简 realtime 配置。

#### 面试追问

**Q：重连期间怎么防止背压？**
`reconnect_loop()` 每次重试间隔 3 秒，这 3 秒内不断 `pop` 排空 in_queue（stream_queue 的源头）。虽然数据被丢弃，但下游队列不会满，EncodeThread 的背压不会传到 CaptureThread。

**Q：为什么 SPS/PPS 只在第一个关键帧出现？**
MPP 硬件编码器的设计行为。它假设一次编码会话中 SPS/PPS 不变（分辨率、profile/level 固定），后续帧只需要携带 slice 数据，不需要重复发参数。这对直播场景的效率是好事（省带宽），但重连时必须靠缓存恢复。

**Q：H.264 vs H.265 怎么选的？**
FLV/RTMP 协议标准没有给 HEVC 分配视频编码器 ID。标准 RTMP 生态（SRS、通用播放器）默认只认 H.264。某些平台有私有 Enhanced RTMP 扩展支持 H.265，但不是通用标准。硬件上板子 MPP 支持 H.265，选 H.264 纯粹是协议兼容性原因。场景上也不需要 H.265：局域网推流带宽不是瓶颈，H.265 编码更复杂反而可能增加延迟。

**Q：GOP=8 是怎么确定的？为什么用 CBR 而不是 VBR？**
GOP（Group of Pictures）=8 在 14.6fps 下对应约 547ms 一个关键帧（I 帧）。选取依据：(1) 太大（如 30+）→ 关键帧稀疏，断线重连或首次拉流时需等很长时间才能解码出第一帧；(2) 太小（如 1，纯 I 帧）→ 每帧完整编码，码率暴涨。GOP=8 是低帧率+实时性场景的经验折中——也是 SRS `gop_cache` 周期约 547ms 冻帧的根因（见 SRS gop_cache 排查弯路）。CBR（恒定码率）vs VBR：RTMP 直播接收端（播放器缓冲区、SRS 中继）假设码率相对稳定，CBR 防止复杂场景瞬间码率暴涨导致播放器缓冲不足卡顿；VBR 画质/码率比更高效但更适合本地录像，不适合实时推流场景。

**Q：为什么设置 `AVFMT_FLAG_FLUSH_PACKETS` 和 `max_delay=0`？去掉会怎样？**
两个参数都是为了压低端到端延迟：`AVFMT_FLAG_FLUSH_PACKETS` 让 FFmpeg 每次 `av_write_frame` 后立即触发 I/O 写出，不在 FFmpeg 内部缓存等凑满缓冲区再发；`max_delay=0` 告知 muxer 不做重排序等待（H.264 流已有 PTS/DTS，muxer 层无需额外排序）。去掉这两个选项：FFmpeg 会在内部积攒一批包再一起发送，推流端延迟会上升数百毫秒甚至数秒，等价于在流水线末端人为加了一个缓冲区，与低延迟推流的目标相矛盾。

---

### 事故链四：交叉编译 ABI 兼容性（BUG-001 + BUG-002）

#### 核心问题

交叉编译时，**编译链用的头文件版本必须与板子上运行的 .so 版本完全一致**，否则编译不报错但运行崩溃或符号找不到。

#### BUG-001：paho MQTT 符号找不到

现象：
```
./AlertGateway: undefined symbol: _ZN4mqtt15connect_options17set_clean_sessionEb
```

原因：WSL 本地编译链接的是从源码 build 的新版 paho-mqtt-cpp，板子上安装的是 `libpaho-mqttpp3 1.2.0` 旧版。新版头文件引入了旧版 .so 里没有的符号。

修复：从 paho git 仓库 checkout v1.2.0 标签提取头文件，从板子 scp .so 回来做链接存根。

#### BUG-002：FFmpeg 头文件版本不匹配导致段错误

现象：gdb 定位到 `avformat_new_stream()` 内部崩溃。

原因：sysroot 里有两套 FFmpeg——系统自带 4.4.2 头文件 + ffmpeg-rockchip 6.1 运行库。CMake include_directories 顺序让编译器用了 4.4.2 的头文件，但链接的是 6.1 的 .so。FFmpeg 6.1 的 `AVFormatContext`/`AVStream` 结构体布局和 4.4.2 不同，字段偏移错误导致访问非法内存。

修复：ffmpeg-rockchip 的头文件路径加到系统头文件之前，保证版本一致。

#### 经验总结

- 头文件版本必须匹配 .so 版本，没有例外
- 最安全的做法：从板子 scp .so 回来做链接存根，从对应 git tag 提取头文件
- 板子上存在多版本的库时，`include_directories` 的顺序决定用的是哪套头文件，顺序错编译不报错但运行崩溃

#### 面试追问

**Q：如果必须换一个库（比如 OpenCV），怎么避免这种问题？**
两种情况分开讨论：(1) 用预编译包 → 和 paho/FFmpeg 同一机制，头文件必须匹配板子上的版本。(2) 自己交叉编译 → ABI 问题转移为依赖链问题：OpenCV 的传递依赖（libjpeg/libpng/zlib）要和板子系统已装版本对齐，且 OpenCV 编译配置项极多，配错一项就出问题。两种方式都不简单，没有银弹。

---

### 事故链五：V4L2 采坑实录

这一块没有 BUG-005 到 BUG-009 那种"一条链串三个 bug"的密集事故，但有几个实实在在踩过的坑。

#### 坑 1：S_PARM 帧率假协商

代码设 `VIDIOC_S_PARM` 请求 30fps，驱动返回成功（不报错），但实测稳定输出只有 **14.6fps**。`v4l2-ctl --list-formats-ext` 查能力表，640×480 YUYV 驱动声明支持 30/25/20/15/10/5fps 共 6 档，但实际只能稳定吐出 14.6fps。V4L2 的帧率设置是"协商"不是"命令"——驱动接受请求但实际能否跑满要看 sensor 能力。

教训：`VIDIOC_S_PARM` 的返回值不代表实际帧率，编码端 PTS 节奏依赖真实帧率。BUG-006 中 RTMP 延迟 15s 的排查就受此影响：配置文件最初写了 fps=30 但实际只有 14.6，PTS 推进速度和真实帧率不匹配。config.json 已修正为 `"fps": 15`。

#### 坑 2：设备路径固定死的隐患

`config.json` 里写死 `/dev/video20`，内核按 USB 枚举顺序分配路径。换个口或换个启动顺序路径就变。这个项目是固定部署，所以目前没触发，但如果需要在不同 USB 口间移动设备就是问题。

#### 坑 3：画框越界导致堆崩溃（BUG-003）

```cpp
// 修复前
static void draw_hline(uint8_t* rgb, int w, int x0, int x1, int y, ...) {
    if (y < 0) return;   // 缺少 y >= h 的检查
```

`y = 480`（刚好等于图像高度）时写入位置超出 `rgb_data` 末尾，破坏堆块元数据，触发 `double free or corruption`。YOLOv8 后处理的坐标变换后，`y2` 可能恰好等于 `frame.height`。

修复：加 `y >= h` 检查——对 YUV/RGB 缓冲区的像素操作，x/y 必须同时检查上下界。

#### 坑 4：USB WiFi + Hub 带宽竞争（BUG-004）

板子通过 USB WiFi 网卡联网，经 USB Hub 接入，而 RTMP 推流 + SSH 管理 + NPU 推理共享同一 USB 总线。现象：`ping` 正常但 SSH 连接超时，因为 ICMP 包极小不会被拥塞控制阻断，但 TCP 握手在带宽竞争时会超时。

修复：插有线网卡走独立通道。嵌入式设备的管理通道和数据通道应该走不同物理链路。

#### 面试追问

**Q：为什么不用 OpenCV VideoCapture 取流？**
几层权衡：(1) OpenCV 默认把 YUYV 转成 BGR（libv4l2 软件 emulation 绕不开），但编码器只认 NV12，变成 YUYV→BGR→NV12 两次转换，跟 BUG-005 修的 RGB 中间格式问题重演。(2) 拿不到精细的 V4L2 参数控制（缓冲区数量、S_FMT 实际协商结果），排查驱动静默改参数时看不到中间状态。(3) 依赖更重，ABI 风险面更大。(4) `VideoCapture::read()` 不带超时，要还原 select+超时检查退出标志的机制还是得自己包一层。这个项目是固定硬件、固定格式管线，直接裸 ioctl 是合理取舍，不是"不会用 OpenCV"。

**Q：S_FMT 设置后怎么确认实际生效了？**
调 `VIDIOC_G_FMT` 回读实际协商结果。代码里提了注释说"驱动会静默修改不会报错"，但没做这个回读检查——是一个已知的盲区。3-4 行代码就能加上。

---

### 事故链六：MQTT 断线重连

MqttThread 使用 Paho MQTT C++ 的 `set_automatic_reconnect(true)` 启用库内置自动重连（未显式配置 `MinRetryInterval`/`MaxRetryInterval`，依赖 Paho 默认的指数退避策略），与 StreamThread 重连的设计思路形成对比：

**StreamThread 重连**（事故发生驱动的重连）
- 先检测到断线 → 手动 close + 循环 open + 缓存 SPS/PPS 恢复
- 重连期间需要手动排空队列防背压

**MqttThread 重连**（库内置的自动重连）
- Paho 客户端内部维护 TCP 连接，断开后自动按指数退避重试
- 不需要手动管理连接状态，重连期间 publish 调用由库内部缓冲/失败回调

#### 面试追问

**Q：`set_automatic_reconnect(true)` 没有设置重试区间参数，有什么潜在问题？**
代码里只调用了 `set_automatic_reconnect(true)` 启用库自动重连，未显式设置 `MinRetryInterval`/`MaxRetryInterval`，依赖 Paho 的默认退避策略。若追求确定性，应显式配置：间隔太短（如 1 秒固定）→ 断线时频繁重建 TCP 连接，加重网络负担；间隔太长（如 5 分钟）→ MQTT 上报中断时间过长，数据丢失窗口大。当前项目的桌面检测场景对 MQTT 实时性要求不高（1 秒上报周期），默认参数可接受，但生产部署前应确认 Paho 默认值并按场景显式设置。

---

### 整体架构回顾

#### 五线程拓扑

```
CaptureThread ──┬──→ [enc_queue=1, 阻塞]   → EncodeThread → [stream_queue=2] → StreamThread
                |                                       ↑ 读
                |                                SharedDetections
                |                                       ↑ 写
                └──→ [infer_queue=2, 非阻塞] → InferThread ──→ [mqtt_queue=16] → MqttThread
```

#### 启动/关闭顺序

启动：下游先于上游（MQTT → RTMP → 编码 → 推理 → 采集），确保消费者已在等待再开始生产。

关闭：上游先于下游，关闭队列触发 `closed_` 标志，各线程自然退出。

#### 设计原则总结

| 原则 | 被哪个事故逼出来的 |
|------|------------------|
| 耗时差异大的步骤必须解耦到不同线程 | BUG-005（NPU 拖死编码帧率）|
| 生产者-消费者之间用容量可控的有界队列 | BUG-005（背压传导）|
| 共享区比队列更适合"读最新值，不要求每帧对齐"的场景 | BUG-005（推理拖编码）|
| 拿到模型后先确认量化类型 | BUG-007（FP16 当 INT8 用）|
| 数值量级差异大的多输出量化必须拆开 | BUG-009（box/cls 共享 scale）|
| 硬件加速器不能完全信任，必须有软件兜底 | InferThread RGA+CPU 双路径 |
| 任何长连接服务必须有重连机制 | BUG-008（SRS 重启后永不复原）|
| 重连必须缓存编码参数 | BUG-008（SPS/PPS 只有首帧）|
| 头文件版本必须匹配 .so 版本 | BUG-001/002（ABI 不兼容）|
| `VIDIOC_S_PARM` 返回值不代表实际帧率 | BUG-006（14.6fps vs 30fps）|
| 对 YUV/RGB 缓冲区的像素操作必须检查 x/y 上下界 | BUG-003（画框越界堆崩溃）|
| 管理通道和数据通道应走不同物理链路 | BUG-004（USB WiFi 带宽竞争）|
| 空闲时主动探测连接状态，不能只靠写失败检测 | StreamThread 心跳（静默断线兜底）|
| 推理频率可通过跳帧参数调节，为未来升级预留旋钮 | infer_every_n_frames 设计 |

---

#### 面试追问（资源管理与关闭流程）

**Q：`stop()` 里为什么要先 `join()` 线程，再释放 RKNN/MPP 硬件资源？顺序颠倒会发生什么？**
`join()` 是等待线程真正执行完 `run()` 退出的同步点。线程的 `run()` 里持续在使用 `ctx_`（RKNN 上下文）和 `input_mem_`（DMA 缓冲区）。如果先销毁这些资源（`rknn_destroy_mem`/`rknn_destroy`），再等 `join`——join 期间 `run()` 线程仍在执行，会操作已被销毁的句柄，变成野指针访问，轻则程序崩溃（segfault），重则 NPU 驱动状态损坏。析构顺序固定为：`running_=false` → `join()` → 释放 `input_mem_`（先于 ctx_，因为它依赖同一个 RKNN context）→ 释放 `ctx_`。析构函数的 `~InferThread()` 调用 `stop()` 作为兜底，也是基于这套顺序。MPP 侧同理：先 `join` 再 `deinit_mpp()`，否则 `mpi_`/`ctx_` 变成悬垂指针。所有持有硬件句柄的工作线程都必须遵守这个析构顺序。

**Q：`main.cpp` 的关闭顺序为什么是"先停采集、再关队列、再停下游"？**
这是"生产者先停、消费者靠队列关闭感知 EOF"的原则。先停 `CaptureThread`（关掉数据来源），再 `close()` 队列（触发 `closed_` 标志），`EncodeThread`/`InferThread` 的 `pop()` 返回 false 后自然退出循环；等它们退出后再关闭 `stream_queue`，`StreamThread` 同理，最后关 `mqtt_queue`，`MqttThread` 退出。如果反过来先停下游——比如先 `stop()` `EncodeThread`，但 `CaptureThread` 还在往已无人消费的 `enc_queue` push，容量为 1 的队列满后 push 超时失败（100ms），`CaptureThread` 会不断丢帧但不会卡死，最终 `infer_queue` 也类似——但下游线程已经退出的情况下 `stream_queue` 满会卡死 `EncodeThread`，导致程序 hang 住无法干净退出。"关闭顺序与数据流方向相反"是多线程有界阻塞队列的经典约束。
