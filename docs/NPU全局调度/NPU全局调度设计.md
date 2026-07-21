# AlertGateway NPU 全局调度设计

日期：2026-07-21
状态：设计已确认，尚未实现

## 1. 背景与问题

当前多路模式会为每个 `ChannelPipeline` 创建一个独立的 `InferThread`、RKNN context 和输入 NPU 内存。每个 context 在初始化时都调用：

```cpp
rknn_set_core_mask(ctx, RKNN_NPU_CORE_0_1_2);
```

也就是说，四个通道会同时请求同一组三个 NPU 核心。`infer_queue` 的“最新帧覆盖”只能避免某一路积压，并不能限制多个 `rknn_run()` 同时提交。因此四路 1080p 压力实测中，单路基线约 26.96 ms 的 NPU 时间上升到约 48--51 ms，实时观看时还出现了 85--88 ms 的尖峰和明显卡顿。

目标不是让四路都以 30 FPS 做检测（现有模型的单路 NPU 原始能力约 37 次/秒，物理上不可能提供 120 次/秒），而是让：

- 视频编码与推流继续按源帧率稳定输出；
- NPU 检测按全局可承受的总速率公平分配；
- 每路始终优先检测最新画面，不因排队出现数秒前的旧框；
- NPU 不再发生多个 all-core context 的无序争用；
- 通道数量、限频、掉帧和实际服务率都可观测。

本设计仅调度 AlertGateway 内部的 RKNN 推理，不管理 SRS。SRS 仍由用户手动启动；测试只检查其是否已就绪。

## 2. 设计结论

首个实现采用“**一个全局串行 NPU 执行器 + 每通道一个最新帧邮箱 + 轮询公平调度**”。

```text
通道 1 拉流 ─┐                         ┌─ 编码/推流（不受 NPU 节拍阻塞）
通道 2 拉流 ─┼─> 每通道 latest mailbox ─┤
通道 3 拉流 ─┤                         └─ SharedDetections（最近一次检测结果）
通道 4 拉流 ─┘
                                      ↓
                         NpuInferenceScheduler
                  过期淘汰 + 每路限频 + Round-Robin 选路
                                      ↓
                   单一 NPU worker / 单一 RKNN context
                   RKNN_NPU_CORE_0_1_2，任一时刻仅一个 rknn_run
                                      ↓
                         后处理、ROI/Tiling 合并、结果回调
```

选择串行 all-core，而不是立刻把四路 context 固定到 CORE_0/1/2，原因是：

1. 当前已验证的低时延基线来自 all-core 运行；单核模型延迟和吞吐尚无实测，不能把它当成四路方案的前提。
2. RKNN context、SRAM 和驱动层并发的行为不透明；当前实测已经证明多个 all-core context 会造成严重抖动。
3. 串行化给出确定的总预算和可解释的公平性，先解决“卡死/忽快忽慢”。

后续若单核/双核测量显示总吞吐更高，才在相同调度接口下加入固定 core-mask worker 池；它是优化分支，不是第一版依赖。

## 3. 容量与默认目标

单路 4K 非 ROI 实测 NPU 平均为 26.961 ms，理论上约 37.09 次 NPU 执行/秒。为给预处理、后处理、系统抖动和偶发 ROI 复检留余量，第一版按 80% 可持续利用率计算：

```text
global_safe_rate ≈ 37.09 × 0.80 ≈ 29 次 NPU 执行/秒
4 路等权默认目标 ≈ 29 / 4 = 7.25 次检测/秒/路
```

因此，四路默认 `target_fps=7`。这里的 7 FPS 是**检测刷新率**，不是结果视频帧率；结果视频仍可维持输入配置的 30 FPS，并以最近有效检测结果叠框。

实际目标要以首版的 120 秒四路测试为准。若 P95 NPU 运行时间、过期率或温度显示余量不足，优先下调全局预算或每路目标，而不是恢复并发 `rknn_run()`。

## 4. 职责划分

### 4.1 `NpuInferenceScheduler`（进程级单例）

负责跨通道的准入和执行顺序：

- 注册/注销通道；
- 每通道只保存一个待处理帧，后来的帧覆盖旧帧；
- 根据每通道下一次允许执行时间做限频；
- 丢弃超过 `max_frame_age_ms` 的帧；
- 从可执行通道中按 Round-Robin 选择一个；
- 在唯一 worker 线程里完成一次 RKNN 执行；
- 记录每通道和全局的排队、淘汰、NPU 时延、端到端时延及利用率。

它不做 RTMP 拉流、MPP 编码、MQTT 网络发送或 UI/SRS 管理。

### 4.2 `InferThread`（每通道保留）

`InferThread` 不再持有长期运行的 RKNN context，也不直接调用 `rknn_run()`。它继续承担通道业务语义：

- 从原有 `infer_queue` 取帧并提交给 Scheduler；
- 接收完成回调；
- 对本通道执行 `SharedDetections` 更新、ROI filter、MQTT 摘要和缩略图逻辑；
- 输出通道级统计。

为避免同一通道完成回调重入，单通道最多允许一个已提交/执行中的 job；在执行期间到达的帧仍只更新该通道 mailbox。

### 4.3 `NpuExecutor`（Scheduler 内唯一 worker）

worker 独占 RKNN runtime 资源：

- 仅在该线程创建、使用和销毁 RKNN context、input memory 及 output buffer；
- 首版要求多通道的模型路径、输入契约、输出布局相同，启动时校验；
- context 使用 `RKNN_NPU_CORE_0_1_2`，但全进程同一时刻至多一个 `rknn_run()`；
- 执行前做 RGA/CPU 预处理与 cache sync，执行后取得输出；
- 返回原始输出或已经完成的检测结果。首版为减少跨线程 RKNN buffer 生命周期复杂度，建议在 worker 内完成 RKNN 输出获取和 YOLO 后处理，再把普通 `std::vector<Detection>` 回调给通道。

每个 context 均不得跨线程使用；第一版只有一个 worker，因而天然满足这一约束。

## 5. 调度算法

### 5.1 数据结构

每个注册通道维护：

```text
ChannelState {
  channel_id
  weight = 1
  target_fps
  next_eligible_at
  latest_frame                 // 可为空；只保留一帧
  submitted / running          // 至多一个正在执行的 job
  counters: received, replaced, throttled, stale_dropped,
            dispatched, completed, failed
}
```

所有状态由 Scheduler mutex 保护；worker 执行 NPU 时不持有该 mutex。完成后以 channel id 回投结果，若通道已经注销则静默丢弃结果。

### 5.2 提交与覆盖

1. `InferThread` 从原来的 `infer_queue` 取到帧后，调用 `submit(channel_id, frame)`。
2. 若该通道已有未开始执行的 `latest_frame`，用新帧替换并令 `replaced++`。
3. 若通道正在执行，仍允许保存一个更新的 `latest_frame`；不会再堆积第二、第三帧。
4. Scheduler 条件变量唤醒 worker。

这样排队深度严格受限于“每路一帧 + 至多一个执行中帧”，检测结果不会落后于实时画面太多。

### 5.3 选路规则

worker 空闲时按以下顺序选 job：

1. 清除时间戳早于 `now - max_frame_age_ms` 的 mailbox 帧，计为 `stale_dropped`；
2. 仅考虑有最新帧、未在执行、且 `now >= next_eligible_at` 的通道；
3. 从上次服务通道的下一个通道开始循环查找，选择第一个可执行通道（Round-Robin）；
4. 未找到可执行帧时，休眠到“有新帧、下一通道到期或停止”三者之一；
5. 一次执行结束后设置 `next_eligible_at = dispatch_time + 1 / target_fps`，然后从下一个通道重新开始。

第一版的 `weight` 固定为 1。第二阶段可替换为 deficit round-robin：一个通道权重为 2 时，每个轮次获得两份服务额度，但仍受自身 `target_fps` 与全局总预算限制。不得用“连续优先取同一路”提高权重，否则会重新引入其它路卡顿。

### 5.4 ROI/Tiling 的配额

ROI/Tiling 一帧可能需要多个 NPU 调用，不能让一个通道长时间独占 worker。第一阶段先规定四路并发配置关闭 ROI/Tiling；验证基本调度稳定后再接入。

接入后以“**一次 `rknn_run` = 一个 NPU slot**”计费：全帧、tile 和联合 ROI 复检都必须单独回到 Scheduler 排队。一个帧级聚合 job 只保留其最新 generation；旧 generation 的后续 tile/复检请求在新帧到来后取消。这样可保证某一路 ROI 不会在一帧内连续吃掉两个以上的调度机会。

## 6. 配置契约

新增进程级 `npu_scheduler` 节点；缺失时保持旧行为，避免影响单路生产配置。四路配置必须显式开启。

```json
"npu_scheduler": {
  "enabled": true,
  "mode": "global_serial",
  "core_mask": "0_1_2",
  "global_target_fps": 29,
  "max_frame_age_ms": 250,
  "stats_interval_sec": 10
}
```

每个 channel 的 `model` 节点新增可选字段：

```json
"model": {
  "target_infer_fps": 7,
  "npu_weight": 1
}
```

校验规则：

- `enabled=true` 时，所有 channel 必须使用相同的 `model.path`、`output_layout`、模型输入尺寸和预处理契约；不满足则启动失败并明确报出字段差异。
- `global_target_fps > 0`、`target_infer_fps > 0`、`npu_weight >= 1`、`max_frame_age_ms >= 0`。
- 所有 channel `target_infer_fps` 之和不得大于 `global_target_fps`；否则启动失败，要求显式降低目标或提高已经测量验证过的全局预算。
- 开启 scheduler 时，`infer_every_n_frames` 不再作为跨通道限流手段；保留兼容解析但启动日志提示其被 scheduler 的时间限频取代。

## 7. 生命周期与故障处理

启动顺序：`main` 解析全部 channels → 校验调度配置与模型契约 → 创建并启动 Scheduler/worker → 创建各 `ChannelPipeline` 并注册 `InferThread` → 启动视频源。

停止顺序：先停止各视频源 → 阻止 Scheduler 接收新帧 → 丢弃尚未派发的 mailbox 帧并唤醒 worker → 等待至多一个运行中的 RKNN job 完成 → 注销通道 → 销毁 RKNN context。不得在 `rknn_run()` 期间销毁 input memory/context。

某次预处理、RKNN 或后处理失败时，只计该 channel 的 `failed`，释放运行标记并继续调度其它通道。连续失败达到阈值时将该 channel 标记为 unhealthy、停止向它派发，同时保留其它通道；最终由主进程记录可诊断错误并按现有策略决定是否退出。

## 8. 可观测性

每 10 秒输出一条全局统计和每通道统计，至少包含：

```text
[NpuScheduler] global dispatched=... completed=... npu_avg_ms=... npu_p95_ms=...
  busy_ratio=... queue_wait_avg_ms=... queue_wait_p95_ms=... stale_dropped=...
[NpuScheduler] channel=1 target=7.0 actual=... mailbox_replaced=...
  throttled=... stale_dropped=... wait_p95_ms=... e2e_p95_ms=... failures=...
```

`actual` 是成功完成的检测次数/秒；`queue_wait` 是进入 scheduler 至开始执行的等待；`e2e` 是输入帧 PTS/单调时间至结果完成。编码/推流继续沿用 `EncodeStats` / `StreamStats`，以区分“检测降频”与“视频输出卡顿”。

## 9. 实施步骤

1. 新增 `NpuInferenceScheduler` 和单一 `NpuExecutor`，仅支持同模型、非 ROI/Tiling、多通道模式。
2. 将 `InferThread` 的 RKNN 生命周期和 `rknn_run()` 移入 executor；保留通道级业务回调与现有单路直连模式。
3. 增加 JSON 解析、契约校验、统计和单元级公平性测试（可用假 executor 模拟固定耗时）。
4. 交叉编译后，在用户手动启动的 SRS 上依次验证单路兼容、双路、四路 120 秒。
5. 用实测总吞吐调整 `global_target_fps` 和四路 `target_infer_fps`，随后才接入 ROI/Tiling slot 化调度。

不在此阶段进行：SRS 自启动改动、RTMP 地址规则改动、MPP/编码调度重写、无实测依据的单核绑定。

## 10. 验收标准

四路 1080p、ROI/Tiling 关闭、结果视频 30 FPS 的 120 秒验证应同时满足：

- 四路结果流均持续有帧，`EncodeStats`/`StreamStats` 无持续队列积压，`enc_drop/out_drop/write_fail=0`；
- 每路实际检测率接近配置目标，等权通道之间不应长期出现明显饥饿；
- NPU `p95` 不再出现当前并发方案的 85--88 ms 级争用尖峰；
- `max_frame_age_ms` 生效，过期帧被计数而非延迟执行；
- 日志能解释每一路的替换、限频、过期和失败；
- 停止时不遗留 AlertGateway/FFmpeg 测试进程，且不启动、停止或重启用户的 SRS。

通过后，四路模式的产品语义应明确表述为“4 路 30 FPS 视频转推 + 约 7 FPS/路公平实时检测”，而不是“4 路 30 FPS 检测”。
