# NPU 全局调度开发计划

日期：2026-07-21
对应设计：[NPU全局调度设计.md](NPU全局调度设计.md)
范围：四路同模型、ROI/Tiling 关闭时的全局串行 NPU 调度；不改 SRS 管理方式

## 0. 完成定义

本计划完成后，四路模式不再由四个 `InferThread` 同时调用 `rknn_run()`。进程内只存在一个实际使用 all-core NPU 的 RKNN 执行 worker；四路以“每路最新帧、等权轮询、约 7 FPS/路检测”的方式共享它。编码与 RTMP 推流仍按源帧率独立运行。

本期不接入 ROI/Tiling、不改变 RTMP 地址规则、不启动/停止/重启 SRS，也不实现单核绑定或多 worker NPU 池。

## 1. 实施顺序总览

| 阶段 | 产物 | 完成判定 |
|---|---|---|
| P0 | 接口与兼容配置 | 默认单路行为不变，开启条件明确 |
| P1 | 可测试的调度器骨架 | 最新帧覆盖、限频、Round-Robin 可由主机测试 |
| P2 | 单一 RKNN 执行器 | RKNN context 仅归 worker 所有 |
| P3 | 通道接入与生命周期 | 多通道接入调度器且可安全停止 |
| P4 | 指标、日志、配置 | 可量化公平性、排队和实际吞吐 |
| P5 | 编译与分层验证 | 单路、双路、四路实测全部通过 |
| P6 | ROI/Tiling 后续设计入口 | 只建立接口/待办，不提前实现 |

每完成一个阶段，先运行对应验证，再进入下一阶段；任何阶段失败都先修复该阶段，不把问题带入板端四路测试。

## 2. P0：配置模型与接口边界

### 任务

1. 在 `src/infer/` 新建 `NpuInferenceScheduler.hpp/.cpp`，先只定义接口与数据结构，不接 RKNN：

   ```cpp
   struct NpuSchedulerConfig;
   struct NpuChannelConfig;
   struct NpuInferenceResult;
   class NpuInferenceScheduler;
   ```

2. 在 `ModelConfig` 增加：

   ```cpp
   int target_infer_fps = 0;  // 0 表示未启用全局调度时沿用旧行为
   int npu_weight = 1;
   ```

3. 在 `main.cpp` 或单独的配置模块解析进程级 `npu_scheduler` 节点，字段为 `enabled`、`mode`、`core_mask`、`global_target_fps`、`max_frame_age_ms`、`stats_interval_sec`。
4. `ChannelPipeline` 只接收已经创建好的 Scheduler 引用/共享指针；不让每条 pipeline 自行创建 scheduler。
5. 启动前校验：开启时必须是 `mode=global_serial`，所有 channel 的模型路径、输出布局、输入尺寸和预处理契约一致；各通道目标 FPS 之和不得超过全局预算。
6. `npu_scheduler` 缺失或 `enabled=false` 时，保持当前 `InferThread` 独立 RKNN context 的旧单路兼容路径。

### 验证

- 用 `jq empty` 校验既有配置和新增四路调度候选配置。
- 增加一个错误配置样例或测试：不同模型路径、目标总和超预算、非法 mode 都必须在启动前失败并给出具体字段。
- 不改动生产 `config/config.json` 的有效默认行为。

### 退出条件

配置解析、依赖注入和校验可编译；此时尚不能实际把帧交给 Scheduler。

## 3. P1：实现无 RKNN 依赖的公平调度器

### 任务

1. Scheduler 为每个已注册通道维护一个 `ChannelState`：最新帧、运行标志、下一次允许执行时间、计数器和完成回调。
2. 实现 `register_channel()`、`unregister_channel()`、`submit()`、`start()`、`stop()`。
3. `submit()` 的规则：同一通道尚未派发的帧被新帧覆盖，计入 `mailbox_replaced`；同一通道运行期间允许保留一帧新 mailbox。
4. worker 取任务时，先淘汰超过 `max_frame_age_ms` 的帧，再从上次服务通道的下一个通道开始 Round-Robin 查找。
5. 派发后按 `target_infer_fps` 设置 `next_eligible_at`；没有可派发任务时以条件变量等待新帧、下一个到期时间或停止信号。
6. 为了主机测试，Scheduler 接收可注入的 `Executor` 抽象；测试 executor 只 sleep 固定毫秒并返回伪结果，不包含 RKNN/RGA。

### 测试项

- 单通道：连续提交 100 帧，执行帧应向最新帧收敛，不能无界积压。
- 四等权通道：持续提交时，完成次数差不超过一个调度轮次。
- 限频：配置 7 FPS 时，相邻派发间隔不小于约 1/7 秒（允许调度误差）。
- 过期：人为提交旧时间戳帧，必须只增加 `stale_dropped`，不进入 executor。
- 停止：worker 等待和 executor 执行期间都能退出；未派发邮箱帧被丢弃且无回调重入。

### 退出条件

主机侧调度测试稳定通过，且没有 RKNN 资源或板端依赖。此阶段可证明算法正确性，但不能证明 NPU 性能。

## 4. P2：把 RKNN 执行收敛到单一 worker

### 任务

1. 从 `InferThread` 抽取与 RKNN 绑定的逻辑到 `NpuExecutor`：模型加载、`rknn_init`、`rknn_set_core_mask`、input memory 创建/绑定、预处理、cache sync、`rknn_run`、outputs_get/release 和后处理。
2. Executor 只由 Scheduler worker 线程创建、调用和销毁；禁止 `InferThread` 保存或访问 `rknn_context`、`rknn_tensor_mem`。
3. 首版固定 `RKNN_NPU_CORE_0_1_2`，并增加运行时断言/日志：任一时刻只允许一个执行中的 NPU job。
4. 输出 worker 已完成的普通 `std::vector<Detection>` 和各阶段耗时；不得把 RKNN output 指针、input memory 或 context 跨线程传回。
5. 保留旧的非调度路径，便于单路 A/B 对比与紧急回退。

### 验证

- 交叉编译 `AlertGateway`、`infer_camera_smoke`、`rknn_benchmark`、`image_processing_smoke`。
- 板端单路短跑：调度开启和旧路径分别运行同一输入，检测类别/框坐标按既有容差对齐，且无 RKNN 生命周期、内存同步或停止错误。
- 日志应只出现一次 `rknn_init` / `Zero-copy input bound`，而不是每通道一次。

### 退出条件

全局 worker 可以稳定完成单路真实 RKNN 推理，停止时没有 context/input memory 释放竞态。

## 5. P3：接入 `InferThread`、`ChannelPipeline` 与安全生命周期

### 任务

1. Scheduler 开启时，`InferThread::run()` 从原有 `infer_queue` 取帧后改为 `submit(channel_id, frame)`，不直接执行 NPU。
2. 完成回调切回该通道的串行上下文，继续执行现有的 `SharedDetections::update()`、ROI filter（本期关闭）、缩略图（本期关闭）和 MQTT 摘要。
3. 明确 generation/帧时间戳：通道在执行期间收到新帧时，旧结果仍可完成，但只能更新对应帧的检测快照；不能错误覆盖更晚的结果。
4. 调整启动顺序：`main` 创建并启动 Scheduler 后，再创建/注册 pipelines，最后启动视频源。
5. 调整停机顺序：停止视频源 → 禁止 Scheduler 新提交 → 丢弃待处理 mailbox → 等待当前唯一 NPU job → 停止/注销 InferThread → 销毁 Scheduler。
6. 确保任一路推流源断开、单通道停止或 MQTT 失败时，不会阻塞其它通道调度。

### 验证

- 单路调度模式运行、SIGINT/timeout 停止、重复启动失败路径均无死锁。
- 两路中断其中一路输入后，另一路仍持续获得调度。
- 程序退出后，板端 `pgrep` 不残留本次 AlertGateway 测试进程；不对 SRS 做任何启动、停止或重启。

### 退出条件

调度器、通道 pipeline 和现有编码/推流链可以安全协作与停止。

## 6. P4：配置、观测与候选运行文件

### 任务

1. 实现每 10 秒全局和每通道统计：`dispatched`、`completed`、`actual_fps`、`mailbox_replaced`、`stale_dropped`、`throttled`、`failed`、NPU 平均/P95、排队等待平均/P95、端到端 P95、busy ratio。
2. 将 Scheduler 统计与已有 `EncodeStats` / `StreamStats` 分开输出，避免把检测限频误判为推流卡顿。
3. 新建独立候选配置，例如 `config/config_multi_4ch_scheduler_candidate.json`：

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

   四个 channel 均设置 `model.target_infer_fps=7`、`npu_weight=1`，并明确关闭 ROI/Tiling。
4. 在启动日志打印生效配置、通道目标、总预算、实际调度模式和回退模式。

### 验证

- 配置生效日志能逐项确认；四路目标和全局预算通过启动校验。
- 人工检查一段日志，可计算每路检测服务率和公平性，且能区分新帧替换、限频与过期淘汰。

### 退出条件

出现性能问题时，不需要猜测是哪一路抢占，可只依据日志定位。

## 7. P5：分层构建与实流验收

### 7.1 主机与交叉编译

1. 运行格式检查：`git diff --check`。
2. 交叉编译全部既有目标；若新增 scheduler 测试目标，一并编译并在主机执行无 RKNN 的算法测试。
3. 校验未开启 Scheduler 的单路配置仍能通过原有配置解析。

### 7.2 板端单路回归

1. 使用相同输入分别跑旧路径与 Scheduler 路径 60 秒。
2. 对比检测输出、NPU 均值/P95、编码 FPS、`enc_drop/out_drop/write_fail`。
3. 若单路 Scheduler 路径的 NPU P95 显著恶化或检测结果不一致，停止后续多路测试，先修复 P2/P3。

### 7.3 双路预验收

1. 用户手动启动 SRS；测试脚本仅检查 API/RTMP 就绪，设置 `NO_PROXY/no_proxy=192.168.0.168,192.168.0.200`。
2. 两路各配置 14 FPS，运行 120 秒。
3. 验收：两路均有稳定结果流、检测率接近目标、编码/推流不持续掉帧、NPU P95 无显著争用尖峰。

### 7.4 四路正式验收

1. 用户手动启动 SRS，并准备 `dual_a`、`dual_b`、`dual_c`、`dual_d` 四个源；结果地址必须为固定模板 `alertgateway_channel_1` 至 `_4`。
2. 使用候选配置运行 120 秒，ROI/Tiling 关闭。
3. 每 10 秒采集 Scheduler、Encode、Stream 统计；测试结束后抓取 SRS API 快照和板端日志。
4. 按如下门槛判定：

   | 指标 | 门槛 |
   |---|---|
   | 结果视频 | 四路持续有帧，按源配置约 30 FPS |
   | 检测刷新 | 每路接近 7 FPS；等权路间不长期饥饿 |
   | NPU 抖动 | 不再出现旧并发方案 85--88 ms 的争用尖峰 |
   | 编码/推流 | `enc_drop=0`、`out_drop=0`、`write_fail=0`，无持续队列积压 |
   | 时效 | 过期帧只计数不执行，P95 排队/端到端时延可解释 |
   | 收尾 | AlertGateway/测试 FFmpeg 均退出；SRS 保持用户原有运行状态 |

5. 若四路实际可持续总检测率低于 29 次/秒，先按日志和温度确定稳定全局预算，再等比例下调四路目标；不得通过恢复四个 all-core context 并发来“提高”数字。

## 8. P6：ROI/Tiling 接入前置条件（后续，不在本期编码）

只有 P5 四路验收通过后才能开始。届时把全帧、tile、联合 ROI 复检都建模为一个 `NpuSlotRequest`；每一次 `rknn_run` 都回到 Scheduler 排队。帧级 generation 变化后取消旧帧尚未执行的 tile/复检请求，以保证一个 ROI 通道不会连续占满 worker。

开始该阶段前需补充单独设计和测试矩阵，重新评估四路检测目标；不能直接沿用非 ROI 的 7 FPS 结论。

## 9. 交付清单

- `src/infer/NpuInferenceScheduler.hpp/.cpp`
- 必要的 RKNN executor 抽取文件（命名在 P2 实现时确定）
- `InferThread`、`ChannelPipeline`、`main.cpp`、`CMakeLists.txt` 的增量修改
- Scheduler 单元/算法测试目标
- `config/config_multi_4ch_scheduler_candidate.json`
- 四路实测日志与结果报告
- `PROJECT_MEMORY.md` 中的已验证结果与下一步更新

代码和实测完成前，本文件的状态始终是“计划”，不应把默认 29/7 FPS 当作已经验证的运行能力。
