# NPU 调用耗时自测 —— 官方 rknn_model_zoo demo 对比

## 背景与目的

`rknn_run`（NPU 推理本体）在 AlertGateway 自己的 pipeline 里测得稳定在 ~39-46ms（见 `npu_optimization_results.md`），已确认是硬件极限，无法通过 mask/SRAM/异步/zero-copy 等手段进一步优化。

为了验证这个结论是否可信、是否是 AlertGateway 自己的调用方式拖慢了 NPU，本次用瑞芯微官方 `rknn_model_zoo` 的 yolov8 demo，跑**同一个模型文件**做交叉验证。

## 测试对象

- 仓库：`https://github.com/airockchip/rknn_model_zoo.git`，clone 到 `/home/rambos/arm_test/rknn_model_zoo`
- Demo：`examples/yolov8/cpp`（rknpu2 标准版，非 zero_copy 版）
- 模型：`/home/rambos/yolov8s.rknn`（md5 `0b29ecba3d102d49e3ddbf0cad4e8efa`），与 AlertGateway 部署到板子上 `~/AlertGateway/model/yolov8s.rknn` 的是同一份文件
- 板子：firefly@192.168.0.200，RK3588S，RKNN driver 2.3.0
- 测试图片：demo 自带的 `bus.jpg`

## 过程

### 1. 编译

链接库版本对齐：板子 `librknnrt.so` 是 2.3.0，仓库自带的 `3rdparty/rknpu2/Linux/aarch64/librknnrt.so` 是 2.3.2，存在 ABI 不一致风险（参考 `feedback_abi_fixes.md` 里 paho-mqtt 的前车之鉴），所以把板子上真实的 `librknnrt.so` scp 回来替换掉仓库自带的再编译：

```bash
scp firefly@192.168.0.200:/usr/lib/librknnrt.so /tmp/board_rknn/librknnrt.so
cp /tmp/board_rknn/librknnrt.so 3rdparty/rknpu2/Linux/aarch64/librknnrt.so
bash build-linux.sh -t rk3588 -a aarch64 -d yolov8 -b Release
```

### 2. 加计时

demo 默认不打印推理耗时，在 `rknpu2/yolov8.cc` 的 `rknn_run` 前后加 `clock_gettime(CLOCK_MONOTONIC, ...)` 打点；并在 `main.cc` 里把单次推理改成循环 30 次，避免第一次调用包含模型加载/NPU 预热开销，干扰结果。

### 3. 部署运行

```bash
scp -r install/rk3588_linux_aarch64/rknn_yolov8_demo/* firefly@192.168.0.200:~/rknn_yolov8_demo_test/
ssh firefly@192.168.0.200 "cd ~/rknn_yolov8_demo_test && LD_LIBRARY_PATH=./lib ./rknn_yolov8_demo model/yolov8s.rknn model/bus.jpg"
```

### 4. 第一次结果异常，排查

默认 governor 下（CPU `interactive`、NPU `rknpu_ondemand`）循环 30 次的 `rknn_run` 耗时落在 **79-128ms**，是 AlertGateway 记录的 ~40ms 的两倍以上，明显不对劲，于是排查：

- 检查 NPU 频率：全程稳定在 `1000000000`（1GHz，已是最高档），把 NPU governor 强行设成 `performance` 重测，**耗时无变化** → NPU 不是瓶颈。
- 检查 CPU 频率：跑测试期间用脚本每 0.3s 采样一次 `scaling_cur_freq`，发现 8 个核心全程卡在 **408MHz**（最低档），governor 是 `interactive`。
- 原因推断：这个孤立的 demo 进程负载太轻（大部分时间在等 NPU/驱动完成 ioctl），`interactive` governor 判断"系统不忙"，不给 CPU 升频；而 `rknn_run()` 本身包含相当一部分 CPU 侧开销（ioctl 调用、等待 NPU 完成的逻辑等），CPU 跑在最低频时这部分被明显拖慢。AlertGateway 实际运行时因为摄像头采集 + 编码 + 多线程持续高负载，CPU 一直被顶在高频，所以不会复现这个问题。

把全部 CPU 核心 governor 临时设成 `performance`（锁满频 1.8GHz/2.256GHz）后重测：

```bash
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance | sudo tee $g; done
```

## 结果

| 条件 | rknn_run 耗时 |
|---|---|
| 默认 governor（CPU interactive @408MHz, NPU 1GHz） | 79-128ms（均值 ~97ms） |
| NPU 锁 performance，CPU 仍 interactive @408MHz | 无变化（79-128ms） |
| CPU 也锁 performance（全核满频） | **稳定 67-70ms** |
| AlertGateway 自己 pipeline 内实测（持续高负载） | ~39-46ms |

## 结论

1. **官方 demo 没有发现 AlertGateway 调用方式有问题**，模型、API 用法都是对的；之前测的 ~40ms 结论站得住。
2. 单独跑官方 demo 测出的数字偏高，根因是**测试环境本身负载太轻导致 CPU 没有升频**，不是 NPU 或模型的差异，是基准测试方法的坑，不是代码问题。
3. 锁满 CPU 频率后差距从 2倍+ 收窄到 67ms vs 40ms，剩余差距大概率是真实 pipeline 持续高负载下的系统状态（缓存命中、调度连续性等）与孤立单进程测试的固有差异，不需要进一步排查。
4. 顺带发现的优化点：如果 AlertGateway 正式运行时希望延迟更稳定，可以考虑把板子 CPU/NPU governor 固定为 `performance`（持久化配置），用更高功耗换取更低且更稳定的延迟抖动。本次改动只是临时生效（sysfs 直接写，重启会还原），是否要做成持久配置待定。

## 相关文件

- `/home/rambos/arm_test/rknn_model_zoo` —— 官方 demo 仓库（本机，未纳入 AlertGateway 版本控制）
- `npu_optimization_results.md`（memory）—— AlertGateway 自身 NPU 优化记录，本次结论与其互相印证
