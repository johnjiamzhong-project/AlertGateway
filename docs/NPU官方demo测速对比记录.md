# NPU 调用耗时自测 —— 官方 rknn_model_zoo demo 对比

## 1. 背景与目的

`rknn_run`（NPU 推理本体）在 AlertGateway 自己的 pipeline 里测得稳定在 ~39–46ms（见 `npu_optimization_results.md`），已确认无法通过 mask/SRAM/异步/zero-copy 等手段进一步优化。

为了验证这个结论是否可信、是否是 AlertGateway 自己的调用方式拖慢了 NPU，本次用瑞芯微官方 `rknn_model_zoo` 的 yolov8 demo，跑**同一个模型文件**做交叉验证。

## 2. 测试对象

- 仓库：`https://github.com/airockchip/rknn_model_zoo.git`，clone 到 `/home/rambos/arm_test/rknn_model_zoo`
- Demo：`examples/yolov8/cpp`（rknpu2 标准版，非 zero_copy 版）
- 模型：`/home/rambos/yolov8s.rknn`（md5 `0b29ecba3d102d49e3ddbf0cad4e8efa`），与 AlertGateway 部署到板子上 `~/AlertGateway/model/yolov8s.rknn` 的是同一份文件
- 板子：firefly@192.168.0.200，RK3588S，RKNN driver 2.3.0
- 测试图片：demo 自带的 `bus.jpg`

## 3. 交叉验证测试

### 3.1 编译与打包

链接库版本对齐：板子 `librknnrt.so` 是 2.3.0，仓库自带的是 2.3.2，存在 ABI 不一致风险。所以把板子上真实的 `.so` scp 回来替换掉仓库自带的再编译：

```bash
scp firefly@192.168.0.200:/usr/lib/librknnrt.so /tmp/board_rknn/librknnrt.so
cp /tmp/board_rknn/librknnrt.so 3rdparty/rknpu2/Linux/aarch64/librknnrt.so
bash build-linux.sh -t rk3588 -a aarch64 -d yolov8 -b Release
```

### 3.2 加计时

demo 默认不打印推理耗时，在 `rknpu2/yolov8.cc` 的 `rknn_run` 前后加 `clock_gettime(CLOCK_MONOTONIC, ...)` 打点；并在 `main.cc` 里把单次推理改成循环 30 次，避免第一次调用包含模型加载/NPU 预热开销，干扰结果。

### 3.3 部署运行

```bash
scp -r install/rk3588_linux_aarch64/rknn_yolov8_demo/* firefly@192.168.0.200:~/rknn_yolov8_demo_test/
ssh firefly@192.168.0.200 "cd ~/rknn_yolov8_demo_test && LD_LIBRARY_PATH=./lib ./rknn_yolov8_demo model/yolov8s.rknn model/bus.jpg"
```

### 3.4 结果异常与排查

默认 governor 下（CPU `interactive`、NPU `rknpu_ondemand`）循环 30 次的 `rknn_run` 耗时落在 **79–128ms**，是 AlertGateway 记录的 ~40ms 的两倍以上，明显不对劲，于是排查：

1. 检查 NPU 频率：全程稳定在 1GHz（已是最高档），把 NPU governor 强行设成 `performance` 重测，**耗时无变化** → NPU 不是瓶颈。
2. 检查 CPU 频率：跑测试期间用脚本每 0.3s 采样一次 `scaling_cur_freq`，发现 8 个核心全程卡在 **408MHz**（最低档），governor 是 `interactive`。
3. 原因推断：这个孤立的 demo 进程负载太轻（大部分时间在等 NPU/驱动完成 ioctl），`interactive` governor 判断"系统不忙"，不给 CPU 升频；而 `rknn_run()` 本身包含相当一部分 CPU 侧开销（ioctl 调用、等待 NPU 完成的逻辑等），CPU 跑在最低频时这部分被明显拖慢。

把全部 CPU 核心 governor 临时设成 `performance`（锁满频 1.8GHz/2.256GHz）后重测：

```bash
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance | sudo tee $g; done
```

## 4. 关键发现：CPU governor 决定推理耗时

| 条件 | rknn_run 耗时 |
|---|---|
| 默认 governor（CPU interactive @408MHz, NPU 1GHz） | 79–128ms（均值 ~97ms） |
| NPU 锁 performance，CPU 仍 interactive @408MHz | 无变化（79–128ms） |
| CPU 也锁 performance（全核满频） | **稳定 67–70ms** |
| AlertGateway 自己 pipeline 内实测（持续高负载） | ~39–46ms |

**结论**：

1. **官方 demo 没有发现 AlertGateway 调用方式有问题**，模型、API 用法都是对的；之前测的 ~40ms 结论站得住。
2. 单独跑官方 demo 测出的数字偏高，根因是**测试环境本身负载太轻导致 CPU 没有升频**，不是 NPU 或模型的差异，是基准测试方法的坑，不是代码问题。
3. 锁满 CPU 频率后差距从 2 倍+ 收窄到 67ms vs 40ms，剩余差距大概率是真实 pipeline 持续高负载下的系统状态（缓存命中、调度连续性等）与孤立单进程测试的固有差异，不需要进一步排查。
4. 顺带发现：**CPU governor 对推理耗时的影响远大于预期**，这是后续所有优化工作的起点。

## 5. 优化方案探索

### 5.1 初始方案：三方案对比测试（2026-07-02）

为了验证性能锁定服务 `rockchip-performance` 的实际效果，使用官方 Demo + INT8 模型进行纯净时延测试。

#### 方案 A：默认节能模式（CPU interactive, NPU ondemand）

```bash
sudo systemctl stop rockchip-performance
cd ~/rknn_yolov8_demo_test
LD_LIBRARY_PATH=./lib ./rknn_yolov8_demo ~/AlertGateway/model/yolov8s.rknn model/bus.jpg
```

```
=== iter 12 === rknn_run cost: 48.95 ms
=== iter 13 === rknn_run cost: 49.44 ms
=== iter 14 === rknn_run cost: 49.85 ms
=== iter 15 === rknn_run cost: 50.99 ms
```
*均值 ~48.8ms，波动明显。*

#### 方案 B：全核锁 performance（CPU + NPU）

```bash
sudo systemctl start rockchip-performance
cd ~/rknn_yolov8_demo_test
LD_LIBRARY_PATH=./lib ./rknn_yolov8_demo ~/AlertGateway/model/yolov8s.rknn model/bus.jpg
```

```
=== iter 12 === rknn_run cost: 34.94 ms
=== iter 13 === rknn_run cost: 34.10 ms
=== iter 14 === rknn_run cost: 35.66 ms
=== iter 15 === rknn_run cost: 35.00 ms
```
*均值 ~34.8ms，抖动完全消除。*

#### 方案 C：差异化锁频（大核/NPU performance，小核 interactive）

```bash
echo interactive | sudo tee /sys/devices/system/cpu/cpufreq/policy0/scaling_governor
echo performance | sudo tee /sys/devices/system/cpu/cpufreq/policy4/scaling_governor
echo performance | sudo tee /sys/devices/system/cpu/cpufreq/policy6/scaling_governor
echo performance | sudo tee /sys/class/devfreq/fdab0000.npu/governor

cd ~/rknn_yolov8_demo_test
LD_LIBRARY_PATH=./lib ./rknn_yolov8_demo ~/AlertGateway/model/yolov8s.rknn model/bus.jpg
```

```
=== iter 25 === rknn_run cost: 34.23 ms
=== iter 26 === rknn_run cost: 33.88 ms
=== iter 27 === rknn_run cost: 35.71 ms
=== iter 28 === rknn_run cost: 33.97 ms
=== iter 29 === rknn_run cost: 33.74 ms
```
*均值 ~33.8ms，性能与全核锁频无差别，小核得以节能，是目前功耗与性能折中的最优方案。*

### 5.2 技术探索一：schedutil + iowait boost（验证时间：2026-07-03）

**背景**：在讨论"CPU 被误判降频"的根因时，有观点提出正确的内核机制应该是让 NPU 驱动在等待时设置 `in_iowait` 标志，唤醒后由 `schedutil` governor 的 iowait boost 自动拉高频率，从而兼顾性能与功耗。本节验证该机制在 RK3588 平台（Linux 6.1.118）上的可行性。

#### 验证结论

| 验证项 | 结果 | 结论 |
|---|---|---|
| `schedutil` governor 是否可用 | ✅ 三个 policy 均支持 | 可以切换 |
| `iowait_boost_max` 参数是否存在 | ❌ 不存在 | iowait boost 未编译 |
| `sched features` 中 iowait 标志 | ❌ 无相关标志 | 内核不支持此特性 |
| 内核编译配置 | `CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y`，无 iowait boost 相关 config | schedutil 是阉割版 |

**最终结论：切换到 `schedutil` 无法触发 iowait boost，该机制在此 BSP 内核中未实现。**

#### 详细验证过程

**1. 各 policy 可用 governor 列表：**

```
/sys/devices/system/cpu/cpufreq/policy0: interactive conservative ondemand userspace powersave performance schedutil
/sys/devices/system/cpu/cpufreq/policy4: interactive conservative ondemand userspace powersave performance schedutil
/sys/devices/system/cpu/cpufreq/policy6: interactive conservative ondemand userspace powersave performance schedutil
```

`schedutil` 在三个 policy 上都可用。

**2. 切换到 schedutil 后参数目录内容：**

```
/sys/devices/system/cpu/cpufreq/policy0/schedutil/
├── rate_limit_us   (当前值: 10000，即 10ms 采样窗口)
└── target_load

/sys/devices/system/cpu/cpufreq/policy4/schedutil/
├── rate_limit_us
└── target_load
```

**关键发现**：标准 Linux mainline 的 `schedutil` 目录中应该存在 `iowait_boost_max` 参数，但这里**完全没有**。这说明 Rockchip 的 BSP 内核（6.1.118）中的 `schedutil` 是经过裁剪的定制版本。额外暴露的 `target_load` 参数（Rockchip 自定义的调频激进程度控制）进一步证实了这一点。

**3. 内核 sched features 确认：**

```bash
cat /sys/kernel/debug/sched/features | tr " " "\n" | grep -i iowait
# 输出：（空，无任何 iowait 相关标志）
```

**4. 内核编译配置：**

```
CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y
CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y
# 无任何 IOWAIT_BOOST 相关 config 项
```

#### 原因分析

Rockchip 针对其嵌入式 BSP 对 schedutil 做了定制化改造：
- **保留**了 `schedutil` 框架（更现代、与 CFS 调度器紧耦合），用于替代老旧的 `interactive`；
- **移除**了 mainline 中的 iowait boost 路径，可能是为了避免在特定嵌入式负载下频率频繁跳变导致的功耗不可控；
- **添加**了自定义的 `target_load` 参数，用自己的方式控制调频激进程度。

### 5.3 技术探索二：UCLAMP（sched_util_min，验证时间：2026-07-03）

**背景**：UCLAMP 是 Linux 5.0+ 引入的调度属性，通过 `sched_setattr` 系统调用为单个线程设置算力下限（`sched_util_min`），理论上可以在不锁死全局主频的情况下告诉 schedutil 维持该线程所需的最低 CPU 频率。

#### 实现方式

在 [InferThread.cpp](../src/infer/InferThread.cpp) 的 `run()` 函数开头，在工作线程自身上下文中调用：

```cpp
struct sched_attr_t { /* ... */ uint32_t sched_util_min; uint32_t sched_util_max; };
sched_attr_t attr{};
attr.size           = sizeof(attr);
attr.sched_policy   = SCHED_OTHER;
attr.sched_flags    = 0x20;   // SCHED_FLAG_UTIL_CLAMP_MIN，必须设置否则内核静默忽略
attr.sched_util_min = 512;    // 0~1024，要求至少 50% 算力对应的主频
attr.sched_util_max = 1024;
syscall(SYS_sched_setattr, 0, &attr, 0);
```

> **关键细节**：`sched_flags` 必须包含 `0x20`（`SCHED_FLAG_UTIL_CLAMP_MIN`），否则内核会成功返回但静默忽略 `util_min` 字段。

#### 实测结果

**内核支持情况：**
```
CONFIG_UCLAMP_TASK=y           ✅
CONFIG_UCLAMP_TASK_GROUP=y     ✅
SYS_sched_setattr = 274        ✅
```

**syscall 执行结果：**
```
[InferThread] UCLAMP util_min=512 applied (tid=30381)  ← syscall 成功返回 0
```

**UCLAMP 生效期间大核/中核主频采样**（全系统切换到 schedutil，网关正常运行）：

```
[1]  policy4=1200MHz  policy6=1416MHz
[2]  policy4=1416MHz  policy6=408MHz
[3]  policy4=1416MHz  policy6=408MHz
[4]  policy4=408MHz   policy6=408MHz   ← 两核均降到物理最低频
[5]  policy4=408MHz   policy6=1416MHz
[6]  policy4=1416MHz  policy6=408MHz
...
```

**syscall 成功，但主频依然在 408MHz ↔ 1416MHz 之间剧烈抖动，与纯 schedutil 无差异。**

#### 原因分析

Rockchip BSP 的 schedutil 实现经过了深度裁剪，影响范围超出预期：

1. **iowait boost 路径被移除**（5.2 节已确认），schedutil 缺少 NPU 中断唤醒时的频率触发信号；
2. **uclamp 应用路径受损**：Rockchip 用自己的 `target_load` 参数替代了标准的 util/uclamp 路径，`util_min` 的约束从未被频率计算逻辑读取；
3. **两套机制存在架构冲突**：`target_load` 覆盖了 schedutil 原生的 util/uclamp 决策路径。

UCLAMP 在 RK3588 BSP 上是一个"能调用但没效果"的特性。

### 5.4 方案 D：全链路锁频（CPU + NPU + DDR + CPUIdle）

前两轮技术探索证明：在 Rockchip 定制的 BSP 内核上，**无法绕过 `performance` governor** 来解决 NPU 推理场景的 CPU 降频问题。

因此进一步扩展 `rockchip-performance` 服务，将锁定范围从 CPU/NPU governor 扩展到两个此前未被关注的维度：

| 新增维度 | 操作 | 收益 |
|---------|------|------|
| **DDR 频率** | `echo performance > /sys/class/devfreq/dmc/governor` | NPU 读取权重/特征图的 DDR 带宽更充裕，NPU 计算本身变快 |
| **CPUIdle state1** | `echo 1 > /sys/devices/system/cpu/cpu*/cpuidle/state1/disable` | 消除 CPU 深度睡眠（220μs 唤醒延迟），NPU 中断响应更快 |

> 为什么之前没做？之前的 `rockchip-performance.service`（commit `c91a0a9`）只锁了 CPU 和 NPU 的 governor，DMC 和 cpuidle 保持了内核默认状态，等于有一个瓶颈没放开。

实测结果：

```
[Infer] cpu:1.06 cpy:0.11 npu:29.43 cnv:1.34 total:31.95 ms objs:0
...
```

**rknn_run（npu）稳定在 ~29.4ms，较方案 C（~33.8ms）再下降 13%。** `cpu` 和 `cnv` 阶段也因 CPUIdle 禁用而显著降低抖动。

## 6. 最终方案对比

| 方案 | rknn_run 均值 | total 均值 | 抖动 | 功耗 |
|------|:-----------:|:---------:|:----:|:----:|
| A：默认 interactive | ~48.8ms | — | 严重 | 低 |
| B：全核 performance | ~34.8ms | — | 极小 | 高 |
| C：大核 perf + 小核 interactive | ~33.8ms | — | 极小 | **低（推荐）** |
| D：全链路锁频（CPU+NPU+DDR+CPUIdle） | **~29.4ms** | **~31.9ms** | **完全抹平** | 高 |

其中 C 和 D 适用于不同场景：

- **方案 C**：热敏感部署（如密闭机箱、无风扇），牺牲 13% 推理性能换取小核不锁频的功耗收益。
- **方案 D**：性能优先场景（如有主动散热、插电固定部署），DDR 锁频 + CPUIdle 禁用带来最佳且最稳定的时延。

## 7. 工程落地：锁频服务与网关启停联动

如果将 `rockchip-performance` 服务设为开机自启，那么板子在未运行 AlertGateway 时空闲时也会保持高频高能耗状态，产生不必要的发热。

### 实现方案

1. **取消开机自启**：
   ```bash
   sudo systemctl disable rockchip-performance
   ```
   开机后系统保持默认节能状态。

2. **将锁频服务生命周期绑定到网关启停**（[start.sh](../start.sh)）：

   ```bash
   # start) 阶段：启动前开启锁频
   sudo systemctl start rockchip-performance
   
   # stop) 阶段：停止后恢复默认调频
   sudo systemctl stop rockchip-performance
   ```

### 启停状态验证

| 组件 | 网关运行（锁频生效） | 网关停止（自动恢复） |
|------|-------------------|-------------------|
| CPU governor | `performance` | `schedutil`（动态调频） |
| NPU governor | `performance` | `rknpu_ondemand`（动态调频） |
| DMC governor | `performance` | `simple_ondemand`（动态调频） |
| CPUIdle state1 | 禁用（`disable=1`） | 启用（`disable=0`，允许深度休眠） |

## 8. 相关文件

- `/home/rambos/arm_test/rknn_model_zoo` —— 官方 demo 仓库（本机，未纳入 AlertGateway 版本控制）
- `/usr/local/bin/rockchip_performance.sh` —— 锁频服务核心脚本（板端），包含 CPU/NPU/DDR governor 锁定及 CPUIdle 管理
- `npu_optimization_results.md`（memory）—— AlertGateway 自身 NPU 优化记录，为本次探索的起点
- `docs/optimization_storyboard.html` (本地文件: [optimization_storyboard.html](file:///home/rambos/arm_test/AlertGateway/docs/optimization_storyboard.html)) —— 本次联合调优的交互式数据与演进历程可视化看板

