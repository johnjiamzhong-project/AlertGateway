# YOLOv5s vs YOLOv8s NPU 推理耗时对比测试记录

## 背景与目的

当前 AlertGateway 部署的是 YOLOv8s INT8，`rknn_run` 实测 ~38-40ms（见 `npu_optimization_results.md`，结论是"已是 NPU 硬件极限"）。想知道换成更轻的 YOLOv5s 能否降低 NPU 推理耗时，同时在测试过程中发现并纠正了两个测量方法上的坑。

## 测试环境

- 板子：firefly@192.168.0.200，RK3588S，RKNN driver 0.9.8/2.3.0
- PC 端转换：rknn-toolkit2 2.3.2（WSL x86_64）
- 板子端运行：rknn-toolkit-lite2 2.3.2（Python）/ librknnrt.so（C API，原生编译）

## 第一轮：用 Python RKNNLite 测 YOLOv5s（结论有误，已纠正）

1. 模型来源：`rknn_model_zoo/examples/yolov5` 官方 `yolov5s_relu.onnx`，用仓库自带 `convert.py` 转 INT8（默认 `optimization_level`，未拆分输出）：
   ```bash
   python3 convert.py ../model/yolov5s_relu.onnx rk3588 i8 ../model/yolov5s_relu_int8.rknn
   ```
2. 写 `bench_npu.py`，用 `RKNNLite.inference()` 整体计时（set_inputs+run+get 全包在一起，不可拆分）：
   - 板子上跑 50 次：`mean=40.91ms median=40.69ms min=30.83ms max=54.34ms`
3. **当时的错误结论**：这个数字跟 AlertGateway 的 YOLOv8s `rknn_run`(~38-40ms) 差不多，认为两个模型耗时相近。
4. **问题**：`RKNNLite.inference()` 是 Python ctypes 封装的整体调用，包含 `rknn_run` 之外的输入搬运、输出获取、以及 Python/ctypes 调用开销，不能直接等同于 C++ 里单独的 `rknn_run`。拿这个数字去跟 AlertGateway C++ 代码里单独打点的 `npu:` 字段比较，是比错了对象。

## 第二轮：改用 C API 分段计时，纠正测量方法

写 `bench_rknn_run.c`，直接调 RKNN C API，把 `rknn_inputs_set` / `rknn_run` / `rknn_outputs_get` 三段拆开各自用 `clock_gettime(CLOCK_MONOTONIC, ...)` 计时，跑 100 次取平均（前 5 次warmup不计入）。

YOLOv5s 结果（此时板子空闲，无其他进程，CPU governor 是 `interactive` @408MHz）：

```
n_input=1 n_output=3   (3 个检测头：1×255×80×80, 1×255×40×40, 1×255×20×20)
inputs_set : mean=0.50ms
rknn_run   : mean=18.14ms  min=17.34ms  max=18.61ms
outputs_get: mean=7.73ms
total      : mean=26.37ms
```

跟第一轮 Python 测的 40.91ms 相比，差了 14ms+ —— 证实了 Python 封装层本身的开销不可忽略。

## 第三轮：写独立 C++ 程序测 YOLOv8s，做同方法对比（不经过 AlertGateway 代码）

把 `bench_rknn_run.c` 改写成 `bench_rknn_run.cpp`（逻辑不变，仍是裸 C API 调用），测 AlertGateway 实际部署在板子上的同一个模型文件 `~/AlertGateway/model/yolov8s.rknn`（为避免误用 AlertGateway 自身代码路径，复制一份到 `~/yolov8s_bench.rknn` 单独测试）。

结果（同样 CPU `interactive` @408MHz 空闲状态）：

```
n_input=1 n_output=2   (box: 1×4×8400, cls: 1×80×8400 —— BUGS.md 里拆分输出修复后的结构)
rknn_run   : mean=59.41ms  min=53.11ms  max=64.06ms
outputs_get: mean=2.51ms
total      : mean=62.64ms
```

这个数字比 AlertGateway 日志里实际报的 `npu: 38-40ms` 高了 50% 左右，尽管用的是同一个模型文件、同样的标准 API 调用方式——出现异常，进入下一轮排查。

## 第四轮：排查 CPU governor 对 rknn_run 的影响

检查发现：

- NPU devfreq（`/sys/class/devfreq/fdab0000.npu`）频率稳定在 `1000000000`（1GHz，最高档），不是瓶颈。
- CPU 侧（`/sys/devices/system/cpu/cpu*/cpufreq`）8 个核心全部锁在 `interactive` governor、`408000`（408MHz，最低档）——因为板子上此时没有其他负载触发升频。

**假设**：`rknn_run()` 内部除了 NPU 矩阵运算外，还包含 CPU 侧提交任务、等待 NPU 完成的同步开销（ioctl/驱动交互），这部分会随 CPU 主频明显变化。AlertGateway 实际运行时 capture/encode/stream 等多线程持续有 CPU 负载，`interactive` governor 会把频率顶上去，所以日志里的 38-40ms 已经接近高频状态下的真实值；而孤立跑 benchmark 时系统判断"不忙"，CPU 被锁在最低频，拖慢了这部分同步开销。

**验证**（征得用户同意后执行，测试完立即改回，已确认恢复）：

```bash
for c in /sys/devices/system/cpu/cpu*/cpufreq; do echo performance | sudo tee $c/scaling_governor; done
# ...跑 benchmark...
for c in /sys/devices/system/cpu/cpu*/cpufreq; do echo interactive | sudo tee $c/scaling_governor; done
```

YOLOv8s 在 `performance`（1.8GHz）下重测：

```
rknn_run   : mean=35.10ms  min=32.88ms  max=38.92ms   ← 与 AlertGateway 日志 38-40ms 基本一致
outputs_get: mean=0.44ms
total      : mean=35.76ms
```

假设得到验证。

## 第五轮：YOLOv5s 同条件重测，得到最终公平对比

同样在 `performance`（1.8GHz）下测 YOLOv5s：

```
rknn_run   : mean=15.68ms  min=15.39ms  max=15.74ms   ← 抖动几乎消失
outputs_get: mean=2.31ms
total      : mean=18.18ms
```

## 第六轮：真实启动 AlertGateway 重新实测（不再是估算的"~38-40ms"）

之前表格里"AlertGateway pipeline 内实测"那一行的 38-40ms 是早前开发过程中记录的数字（来自 `npu_optimization_results.md`），不是这次测试当场跑的。这次专门把真实服务跑起来重新验证一次，流程：

1. 确认 `~/AlertGateway/AlertGateway` 二进制存在、没有进程在跑。
2. 第一次用 `./start.sh start` 启动，**失败**：`StreamThread` 连不上 RTMP（`192.168.0.168:1935 Connection refused`），程序在拉流连接失败时直接退出，根本没跑到 `InferThread`，CPU 全程停在 idle 408MHz——确认没有产生真实负载。
3. 确认局域网里的 RTMP/MQTT 服务（SRS，跑在 `192.168.0.168`）已启动后，重新 `./start.sh start`，真实摄像头采集 + RGA 预处理 + NPU 推理 + MPP 编码 + RTMP 推流 + MQTT 上报全链路跑起来。
4. 运行期间从 `/tmp/ag.log` 连续取 40 条 `npu:` 字段，同时采样 CPU/NPU 频率。
5. 测完用 `./start.sh stop` 正常关闭，确认进程已退出。

**结果：**

```
npu: mean=44.76ms  min=32.78ms  max=51.29ms  (n=40)

运行期间 CPU 频率: 8 核里只有 4 个核升到 1.2GHz，另外 4 个核仍停在 408MHz（governor: interactive）
运行期间 NPU 频率: 1.0GHz（满频，跟之前一致）
```

### 三组测试条件的区别

三组数的模型和硬件都相同（同一块板子、同一个 `yolov8s.rknn`），唯一变量是当时的 CPU 频率状态，根源是当时"系统负载"不同：

| 条件 | 测试方式 | CPU 频率状态 | 原因 |
|---|---|---|---|
| 条件 1：孤立 benchmark，idle | 独立小程序 `bench_rknn_run_cpp` 单独跑，不开摄像头/RTMP/MQTT | 8 核全部 408MHz | 程序大部分时间在等 NPU，CPU 占用率低，`interactive` governor 判断"不忙"，不升频 |
| 条件 2：孤立 benchmark，performance | 同一个独立小程序，但跑之前手动把 governor 强制改成 `performance`（跑完立刻改回） | 8 核全部锁满 1.8GHz | 人为干预，排除 CPU 频率这个变量，看 NPU/驱动调用本身的最快可能表现 |
| 条件 3：AlertGateway 真实运行 | 真实服务（摄像头+RGA+NPU+MPP+RTMP+MQTT 多线程同时跑），governor 仍是默认 `interactive`，无人为干预 | 4 核到 1.2GHz，另 4 核仍 408MHz | 真实多线程负载自然触发部分核心升频，但没有像条件 2 那样所有核心都顶满，所以频率介于条件 1 和条件 2 之间 |

条件 3 的结果（44.76ms）正好落在条件 1（59.41ms，最慢）和条件 2（35.10ms，最快）之间，且本身波动较大（32.78-51.29ms，接近 20ms 的抖动），说明真实运行时 CPU 频率本身是持续波动的，不是稳定在某个值，`npu:` 也跟着波动，比之前文档里写的"稳定 38-40ms"更接近真实情况的应该是这个有波动范围的区间，而不是一个常数。

## 第七轮：单独测试 NPU governor（排查是否还有空间）

第四轮只动了 CPU governor，NPU 侧一直是默认的 `rknpu_ondemand`。这一轮单独验证 NPU governor 本身是否还能再省一点：

```bash
cat /sys/class/devfreq/fdab0000.npu/available_governors
# rknpu_ondemand dmc_ondemand vop2_ondemand venc_ondemand userspace powersave performance simple_ondemand
```

1. **只改 NPU governor，CPU 仍 idle 408MHz**：
   ```bash
   echo performance | sudo tee /sys/class/devfreq/fdab0000.npu/governor
   ```
   结果：`rknn_run mean=60.21ms`（min 52.59 / max 65.30），跟 baseline 的 59.41ms 几乎没差，在噪声范围内。**单独锁 NPU governor 没用**——因为 `rknpu_ondemand` 本身在有负载时就会自动顶到可用频率上限（1.0GHz），跟 `performance` 锁定的频率是同一个值，governor 类型不是瓶颈。

2. **CPU 和 NPU 两个 governor 都锁 performance**：
   结果：`rknn_run mean=31.42ms  min=31.17ms  max=31.81ms`——比第四轮只锁 CPU 的 35.10ms 还低了一截，且波动范围从 6ms 收窄到 0.64ms。

   推测原因：`rknpu_ondemand` 虽然在持续负载下能顶到 1GHz，但从空闲到顶满需要一点反应时间（governor 重新评估周期），测试头几次调用可能落在频率还没完全升上去的窗口里，拉高了平均值；`performance` governor 从第一次调用开始就锁死最高频，没有这个爬升过程，所以更快也更稳。

3. 测试完成后已将两个 governor 都改回原值并确认生效：
   ```bash
   CPU: interactive, 全部核心已回落到 408MHz（cpu0 短暂残留在 1008000 后续会继续下降）
   NPU: rknpu_ondemand
   ```

## 最终结果汇总

| 模型 | CPU 状态 | NPU governor | rknn_run / npu | outputs_get | total |
|---|---|---|---|---|---|
| YOLOv5s INT8（官方未拆分，3 个检测头输出） | idle, 408MHz | ondemand | 18.14ms | 7.73ms | 26.37ms |
| YOLOv5s INT8 | performance, 1.8GHz | ondemand | **15.68ms** | 2.31ms | 18.18ms |
| YOLOv8s INT8（AlertGateway 部署，box/cls 拆分） | 孤立 benchmark，idle 408MHz | ondemand | 59.41ms | 2.51ms | 62.64ms |
| YOLOv8s INT8 | 孤立 benchmark，idle 408MHz | **performance** | 60.21ms（无变化） | 3.31ms | 64.53ms |
| YOLOv8s INT8 | 孤立 benchmark，performance 1.8GHz | ondemand | 35.10ms | 0.44ms | 35.76ms |
| YOLOv8s INT8 | 孤立 benchmark，**performance 1.8GHz** | **performance** | **31.42ms（最优，最稳）** | 0.31ms | 31.86ms |
| YOLOv8s INT8 | **AlertGateway 真实运行**（4核1.2GHz+4核408MHz） | ondemand | **44.76ms**（32.78-51.29ms） | 含在 cnv 字段 | ~65-83ms（见日志 total 字段） |

## 结论

1. **YOLOv8s 的 `rknn_run`（35.10ms）约为 YOLOv5s（15.68ms）的 2.24 倍**，比两者理论 FLOPs 比值（28.6 / 16.5 ≈ 1.73）差距更大一些，推测多出来的部分来自 YOLOv8s 的 box/cls 拆分双输出（计算图节点更多）以及 `tools/convert_int8.py` 里 `optimization_level=0`（关闭了图融合优化）。换成 YOLOv5s 能在 NPU 算力上省下一半以上，代价是精度通常不如 YOLOv8s（更老的 anchor-based 结构）。
2. **CPU 主频对 `rknn_run` 耗时有显著影响**：idle 408MHz vs performance 1.8GHz，YOLOv8s 差了 50-70%，YOLOv5s 差了 ~14%（受影响幅度不同，可能跟输出张量大小/反量化搬运量有关）。这说明 `rknn_run` 不是纯粹的 NPU 硬件算力耗时，还包含受 CPU 频率影响的同步开销。该发现与本仓库另一份独立记录 `NPU官方demo测速对比记录.md`（用官方 demo 测 YOLOv8s 得到类似结论，但差距是 2 倍以上）互相印证，两次用不同方法在不同时间得到一致的方向性结论。
3. 因此，`npu_optimization_results.md` 里"`rknn_run` ~40ms 是 NPU 硬件极限"的结论需要修正为：**这是 CPU 处于某个中间频率状态下的实测值，不是固定常数**。第六轮现场重测真实 AlertGateway 拿到的是 `npu: mean=44.76ms（32.78-51.29ms 波动）`，比早前记录的"38-40ms"更高、波动也更大，原因是真实运行时 `interactive` governor 只把 8 核中的 4 核顶到 1.2GHz（不是想象中的全核满频）。NPU 计算本身的真实下限需要更精确的手段（如 `rknn_query(RKNN_QUERY_PERF_DETAIL)` 拆出 NPU 占用时间，排除 CPU 同步部分）才能确定。
4. **测量方法的坑**：Python `RKNNLite.inference()` 的封装开销（本次约 14ms）会让跨模型/跨实现的耗时比较失真；要做精确对比，必须用 C API 把 `rknn_inputs_set` / `rknn_run` / `rknn_outputs_get` 分段计时，且要确保被测环境的 CPU 频率状态跟生产环境（如 AlertGateway 持续运行时）一致，否则孤立的 benchmark 数字会显著偏高。
5. **真实生产环境下的耗时不是一个稳定常数，而是有约 20ms 量级波动的区间**，根源是 `interactive` governor 对 CPU 频率的动态调整不是"全有或全无"，而是按需只顶部分核心、且会随时间波动。如果想要更稳定可预测的延迟，可以考虑把板子 CPU governor 固定为 `performance`（用更高功耗换取更低且更稳定的延迟抖动），这点在 `NPU官方demo测速对比记录.md` 里也提到过，目前还未真正落地为持久化配置。
6. **NPU governor 单独锁 performance 没有意义**——`rknpu_ondemand` 在有负载时本来就会自动顶到频率上限（1.0GHz），跟 `performance` 锁定的是同一个频率，所以单独改 NPU governor 测出来的数字（60.21ms）跟默认 ondemand（59.41ms）几乎没区别。但 **CPU+NPU 两个 governor 一起锁 performance 时是目前测到的最优值**（`rknn_run` 31.42ms，波动只有 0.64ms），比只锁 CPU 的 35.10ms 还要再快一点、更稳——推测是 `rknpu_ondemand` 从空闲到顶满有反应延迟，影响了测试头几次调用，`performance` 没有这个爬坡过程。如果要把这个数字落地为生产环境的常态收益，需要把 CPU+NPU 两个 governor 都持久化为 `performance`，目前都只是临时测试、已改回默认值。

## 相关文件

- 本机临时脚本（未纳入仓库版本控制）：`/tmp/bench_npu.py`、`/tmp/bench_rknn_run.c`、`/tmp/bench_rknn_run.cpp`
- 板子上对应文件：`~/bench_npu.py`、`~/bench_rknn_run`、`~/bench_rknn_run_cpp`、`~/yolov5s_relu_int8.rknn`、`~/yolov8s_bench.rknn`（从 `~/AlertGateway/model/yolov8s.rknn` 复制）
- 相关记录：`npu_optimization_results.md`（memory）、`NPU官方demo测速对比记录.md`（本目录，更早一次用官方 demo 验证 CPU governor 影响的独立记录）
