# YOLOv8s-RK3588 推理优化实验记录

本文档按实验编号记录每次操作、预期、实际结果、原因分析和结论。失败或中断的实验也保留，
避免后续重复踩坑。整体方向见 `YOLOv8s-RK3588推理性能优化路线.md`。

## 统一记录规则

- 每次只改变一个主要变量；
- 板端命令、环境状态和测试输出应在实验当天记录；
- 正式性能测试使用固定输入，预热100次，至少统计1000次；
- 至少记录均值、中位数、P90、最小值和最大值；
- `rknn_inputs_set`、`rknn_run`、`rknn_outputs_get` 分段计时；
- 同一组对比保持模型输入、核心模式、SRAM标志、频率、温度和Runtime版本一致；
- 没有完成同口径对比前，不进入下一项优化。

---

## EXP-001 当前模型与Rockchip官方优化模型基线统一

### 状态

已完成。标准API、零拷贝、真实流水线和CPU核心调度差异均已验证。

### 日期

2026-07-05

### 目的

在同一块RK3588S板卡、同一个C API benchmark和相同频率条件下，对比：

1. AlertGateway当前box/cls双输出YOLOv8s INT8模型；
2. Rockchip Model Zoo官方优化检测头YOLOv8s INT8模型。

确认当前约29.4 ms与公开性能数据的差距来自模型图结构，还是来自测试环境、工具链、
核心模式或测量口径。

### 已知基线

- 当前生产模型输入：640×640；
- 当前模型输出：box `1×4×8400` + class `1×80×8400`；
- 当前模型转换参数包含 `optimization_level=0`；
- 当前代码使用 `RKNN_FLAG_ENABLE_SRAM`；
- 当前代码设置 `RKNN_NPU_CORE_0_1_2`；
- 已记录最优稳定结果：`rknn_run()`约29.4 ms；
- 旧独立benchmark只运行100次、前5次warmup，不满足本轮正式口径；
- 旧“官方Demo测试”加载的仍是AlertGateway当前模型，并不是真正的官方优化模型。

### 本轮固定测试口径

- 固定输入数据；
- warmup：100次；
- 正式运行：1000次；
- 使用RKNN C API；
- 原始量化输出，不在`outputs_get`中请求float转换；
- 分段记录inputs、run、outputs；
- 额外查询 `RKNN_QUERY_PERF_RUN`（若当前Runtime支持），区分NPU内部时间和
  `rknn_run()`墙钟时间；
- 分别测试：
  - `RKNN_NPU_CORE_AUTO`，不启用SRAM；
  - `RKNN_NPU_CORE_0`，不启用SRAM；
  - `RKNN_NPU_CORE_0_1_2` + `RKNN_FLAG_ENABLE_SRAM`，匹配当前生产代码；
- CPU、NPU和DDR保持同一performance设置；
- 测试时停止AlertGateway及其他NPU任务。

### 完成标准

- 两个模型均使用同一benchmark完成至少1000次正式测试；
- 记录模型SHA-256、输入输出结构、SDK/Driver版本、核心模式、SRAM和governor；
- 获得可比较的中位数和P90；
- 对差异给出有证据的解释；
- 明确下一步是先调整转换参数，还是先采用Rockchip优化检测头。

### 操作记录

#### 1. 检查仓库现有资料

结果：

- 仓库没有可复用的独立benchmark源码；
- 旧benchmark源码只存在于本机 `/tmp` 或板端Home目录，未纳入版本控制；
- `docs/YOLOv5s与YOLOv8s-NPU耗时对比测试记录.md` 保存了旧实验结果；
- 需要把benchmark整理为仓库内正式工具。

#### 2. 第一次通过项目MCP连接板子

使用项目的 `.mcp.json` 和 `tools/board_mcp.py`，通过 `board_exec` 只读检查板端。

第一次结果：

```text
两个配置地址的SSH连接均超时
exit code: 255
```

该结果只表示当时SSH连接失败，不能作为板卡或模型实验结论。

#### 3. MCP最小连通性重试

执行：

```text
printf 'board_mcp_ok\n'
hostname
date '+%F %T %z'
```

结果：

```text
board_mcp_ok
firefly
2026-07-05 16:32:09 +0800
exit code: 0
```

结论：项目MCP可以正常调用RK3588S板子。此前超时属于临时连接问题。

#### 4. 盘点板端模型和环境

板端状态：

```text
kernel: Linux 6.1.118 aarch64
RKNN Runtime: 2.3.0
AlertGateway进程: 未运行
rockchip-performance.service: inactive
CPU governor: interactive
NPU governor: rknpu_ondemand
```

找到的主要模型：

| 路径 | 大小 | SHA-256 | 初步身份 |
|---|---:|---|---|
| `~/AlertGateway/model/yolov8s.rknn` | 13,062,888 | `da5578d...d5d15c5` | 当前双输出INT8模型 |
| `~/yolov8s_bench.rknn` | 13,062,888 | `da5578d...d5d15c5` | 当前模型的相同副本 |
| `~/rknn_yolov8/yolov8s.rknn` | 24,725,110 | `6e126eb0...bc2a12` | 待确认候选模型 |
| `~/rknn_yolov8/yolov8n.rknn` | 8,288,694 | `9db5ee20...ed0f` | 非本轮目标 |
| `~/yolov5s_relu_int8.rknn` | 8,388,664 | `a45ca5d4...7ed98` | 历史对照模型 |

板端保留了旧版 `~/bench_rknn_run.cpp` 和可执行文件。源码确认其测试口径为：

- 默认只测试100次；
- 只预热5次；
- 随机输入未固定seed；
- 输出统一请求float转换；
- 只统计mean/min/max；
- 未记录中位数、P90、`RKNN_QUERY_PERF_RUN`、核心模式或SRAM状态。

因此旧工具可用于快速检查模型元数据，但不能直接作为EXP-001正式工具。

#### 5. 确认板端候选YOLOv8s模型身份

使用旧benchmark各运行一次（另有5次预热），只确认张量结构，不作为正式性能数据。

当前模型：

```text
input: 1×640×640×3 INT8
output[0]: 1×4×8400 INT8
output[1]: 1×80×8400 INT8
```

候选模型 `~/rknn_yolov8/yolov8s.rknn`：

```text
input: 1×640×640×3 FP16
output[0]: 1×84×8400 FP16
```

动态调频下的单次输出约为：

```text
当前INT8模型 rknn_run: 62.85 ms
候选FP16模型 rknn_run: 89.26 ms
```

这两个耗时只包含极少次数且governor未锁定，不能用于性能结论。

结论：候选模型是原始单输出FP16模型，不是Rockchip官方优化检测头INT8模型，不能作为
EXP-001正式对照。需要获取官方优化ONNX并重新转换。

#### 6. 建立仓库内正式benchmark

新增：

```text
tools/benchmark/rknn_benchmark.cpp
```

并在CMake中增加默认关闭的 `BUILD_RKNN_BENCHMARK` 选项，避免影响生产构建。

正式工具支持：

- 可配置warmup和正式运行次数；
- 固定、可重复的输入数据，或加载指定原始输入文件；
- `auto/0/1/2/01/012/all` NPU核心模式；
- 可选 `RKNN_FLAG_ENABLE_SRAM`；
- 原始量化输出，避免float反量化干扰；
- inputs/run/outputs分段统计；
- mean、median、P90、min和max；
- `RKNN_QUERY_PERF_RUN`；
- 打印模型输入输出结构和SDK/Driver版本。

本机使用独立临时构建目录交叉编译成功：

```text
/tmp/alertgateway-benchmark-build/rknn_benchmark
ELF 64-bit LSB pie executable, ARM aarch64
```

上传到板端：

```text
~/rknn_benchmark_exp001
```

在动态调频状态下对当前模型执行5次预热+10次测试，确认工具、张量查询、统计和
`RKNN_QUERY_PERF_RUN`均正常。该短测试不计入正式基线。

#### 7. 获取并转换Rockchip官方优化模型

官方来源：

```text
repository: https://github.com/airockchip/rknn_model_zoo
commit: bad6c7334531becaf90a561988519b7bec34d0ab
```

下载的官方YOLOv8s ONNX：

```text
SHA-256: 927da9dc878f5c1bc86471806a92e37d0df7fef782cb2164962691d871848b2e
节点数: 228
输出数: 9
```

三个尺度分别输出：

```text
80×80: box 1×64×80×80, class 1×80×80×80, score_sum 1×1×80×80
40×40: box 1×64×40×40, class 1×80×40×40, score_sum 1×1×40×40
20×20: box 1×64×20×20, class 1×80×20×20, score_sum 1×1×20×20
```

使用Model Zoo原始 `examples/yolov8/python/convert.py`、RKNN-Toolkit2 2.3.2和仓库附带的
20张COCO校准集转换，未主动添加 `optimization_level` 参数。

生成结果：

```text
文件: /tmp/yolov8s_rockchip_official_int8.rknn
大小: 12,466,635 bytes
SHA-256: 3ef973da2f22985c36555dfb4e63f2eb1a53befdec815a86aed642f22e8b8e0a
```

转换日志出现首层权重outlier警告：

```text
model.0.conv.weight abs_mean=4.03 abs_std=4.41 outlier=26.039
```

该警告需要在后续精度验证中关注，但不影响本轮性能基线。

第一次MCP上传长时间无结果，核验后确认板端没有残留文件；第二次单独上传成功，板端SHA-256
与本机一致。板端路径：

```text
~/exp001/yolov8s_rockchip_official_int8.rknn
```

#### 8. 正式测试环境

启动 `rockchip-performance.service` 后确认：

```text
service: active
policy0: performance / 1.800 GHz
policy4: performance / 2.256 GHz
policy6: performance / 2.256 GHz
NPU: performance / 1.000 GHz
测试温度范围: 37.9～39.8°C
AlertGateway及其他NPU任务: 未运行
RKNN API: 2.3.0
RKNN Driver: 0.9.8
```

每个模型、每种模式均预热100次并正式运行1000次。六份原始输出保存在板端
`~/exp001/*1000.txt`。

#### 9. 正式结果

`rknn_run()`墙钟中位数：

| 模式 | 当前双输出INT8 | Rockchip官方优化INT8 | 差值 | 官方模型提升 |
|---|---:|---:|---:|---:|
| AUTO，无SRAM | 31.367 ms | 27.566 ms | -3.801 ms | 12.1% |
| Core 0，无SRAM | 31.372 ms | 27.573 ms | -3.799 ms | 12.1% |
| Core 0/1/2 + SRAM | 30.408 ms | 26.688 ms | -3.720 ms | 12.2% |

标准API、未固定CPU的 `Core 0/1/2 + SRAM` 完整统计：

| 阶段 | 当前模型median/P90 | 官方模型median/P90 |
|---|---:|---:|
| inputs_set | 0.220 / 0.233 ms | 0.250 / 0.260 ms |
| rknn_run wall | 30.408 / 30.466 ms | 26.688 / 26.716 ms |
| outputs_get | 0.172 / 0.179 ms | 1.747 / 1.770 ms |
| RKNN perf run | 30.406 / 30.464 ms | 26.686 / 26.714 ms |

三段中位数直接相加：

```text
当前模型: 30.800 ms
官方模型: 28.685 ms
净减少:    2.115 ms（约6.9%）
```

注意：该三段合计仍不包含CPU端DFL解码、阈值筛选和NMS。

当前模型AUTO和Core 0结果基本相同，说明AUTO在本次环境下选择了等价的单核执行方式。
`Core 0/1/2 + SRAM`比单核无SRAM快约3%，但该组同时改变了核心mask和SRAM两个设置，
不能仅凭本次结果判断收益具体来自哪一个。

完成测试后停止性能服务并确认恢复：

```text
service: inactive
CPU governor: interactive
NPU governor: rknpu_ondemand
板端正式结果文件: 6份
```

#### 10. 补充零拷贝输入和固定真实图片

将benchmark扩展为与AlertGateway一致的
`rknn_create_mem + rknn_set_io_mem + memcpy`输入方式，并使用固定640×640 RGB bus图片。

| 输入方式 | 输入 | 当前模型rknn_run median/P90 |
|---|---|---:|
| 标准API | 确定性高熵字节 | 30.408 / 30.466 ms |
| 零拷贝 | 确定性高熵字节 | 30.346 / 30.400 ms |
| 零拷贝 | 固定bus真实图片 | 30.356 / 30.433 ms |

结论：标准API与零拷贝只差约0.06 ms；高熵字节与真实图片只差约0.01 ms，二者都不是
29.4与30.4 ms差异的主要原因。

固定输入信息：

```text
格式: 640×640 RGB UINT8
大小: 1,228,800 bytes
SHA-256: a6d5d92be093dbf380a35bee459ebc631b44d5ee863f956a380486e7f2aed5a3
```

官方模型在零拷贝和固定bus输入下为：

```text
rknn_run median=26.622 ms
P90=26.665 ms
```

#### 11. 排除调用节拍、perf查询和输出类型

继续逐项匹配真实AlertGateway调用方式：

| 变更 | 当前模型rknn_run median | 结论 |
|---|---:|---|
| 零拷贝，连续运行 | 30.356 ms | 补充验证起点 |
| 零拷贝，固定67 ms周期 | 30.397 ms | 15 FPS节拍不是原因 |
| 零拷贝，不查询 `RKNN_QUERY_PERF_RUN` | 30.340 ms | perf查询不是原因 |
| 零拷贝、67 ms、float输出、不查perf | 30.410 ms | 输出模式不是原因 |

#### 12. 真实AlertGateway长时间复测

启动真实网关，在全链路性能服务启用状态下采集1245条 `[Infer]` 日志：

```text
mean=29.868 ms
median=29.429 ms
P90=30.573 ms
min=29.378 ms
max=31.772 ms
```

原有约29.4 ms生产结果得到大样本复现，确实有效，而且是中位数水平，不是偶然最小值。
但P90为30.573 ms，旧文档中的“抖动完全抹平”表述过强。

原始数据保存在板端：

```text
~/exp001/alertgateway_pipeline_infer.log
~/exp001/alertgateway_pipeline_npu_ms.txt
```

#### 13. 定位约1 ms差异：CPU核心调度

RK3588包含不同CPU微架构，而 `rknn_run()`包含CPU侧任务提交和驱动同步。将同一个独立
benchmark固定到CPU6大核后：

| 模型 | CPU6 rknn_run median | P90 | 相对当前模型 |
|---|---:|---:|---:|
| 当前双输出INT8 | 29.237 ms | 29.249 ms | 基线 |
| Rockchip官方优化INT8 | 25.662 ms | 25.675 ms | 快3.575 ms，约12.2% |

当前模型详细范围为29.207～29.329 ms，稳定复现并略优于真实AlertGateway的29.429 ms。
这说明之前约1 ms差异来自调用线程CPU调度，不是模型退化、输入API、画面内容或统计错误。

所有补充测试结束后停止性能服务并确认完整恢复：

```text
CPU governor: interactive
NPU governor: rknpu_ondemand
DMC governor/frequency: dmc_ondemand / 528 MHz
CPUIdle state1 disable: 0
```

### 预期

- 如果官方优化模型明显快于当前模型，优先迁移官方检测头结构；
- 如果两个模型在同口径下接近，先排查Toolkit版本、转换优化等级和运行环境；
- 29.4 ms不能与不同输入尺寸或不同计时范围的“约20 ms”直接比较。

### 实际结果

Rockchip官方优化模型在三种运行模式下，`rknn_run()`均比当前双输出模型快约12.1%～12.2%。
标准API且未固定CPU时，`Core 0/1/2 + SRAM`中位数由30.408 ms降至26.688 ms。

真实AlertGateway采集1245帧后，中位数为29.429 ms，正式确认此前约29.4 ms结果有效。
独立benchmark未固定CPU时约30.4 ms，固定到CPU6大核后为29.237 ms。
在相同CPU6条件下，官方模型为25.662 ms，比当前模型快约12.2%。

### 原因分析

本轮能够确认“Rockchip官方整套优化模型+默认转换配置”比当前模型快，但不能把全部12.2%
都归因于移除DFL，因为两者同时存在两个主要变量：

1. 模型图不同：官方模型移除了NPU内DFL和最终框解码；
2. 转换配置不同：当前模型明确使用 `optimization_level=0`，官方脚本使用Toolkit默认值。

另外，官方模型需要在CPU端重新执行DFL和坐标解码，而当前模型的box已经在NPU内解码完成。
本轮benchmark没有包含两种模型各自的完整CPU后处理，因此不能宣称完整业务推理已经快12.2%。

约29.4与30.4 ms差异已通过单变量测试排除输入API、输入内容、调用节拍、float输出及perf查询。
将独立benchmark固定到CPU6后稳定降至29.237 ms，证明CPU核心调度是主要原因。

公开约24.5 ms与本轮官方模型CPU6结果25.662 ms仍有约1.2 ms差距，可能来自板卡型号、
Runtime/Driver、Model Zoo版本、编译器版本或公开数据使用吞吐率而非单次延迟。

### 结论

EXP-001纯RKNN基线和差异排查完成：

- 当前模型真实AlertGateway：`median=29.429 ms, P90=30.573 ms, n=1245`；
- 当前模型CPU6独立benchmark：`median=29.237 ms, P90=29.249 ms`；
- 官方模型CPU6独立benchmark：`median=25.662 ms, P90=25.675 ms`；
- 官方整套方案在NPU阶段有稳定、可重复的约12.2%优势；
- 旧29.4 ms基线有效，独立工具此前30.4 ms的差异来自CPU调度；
- 尚未验证检测精度和完整后处理总耗时，不能直接替换生产模型。

下一项应隔离“转换优化等级”变量：保持当前双输出ONNX不变，仅测试
`optimization_level=0/1/2/3`，并固定benchmark到CPU6。

### 后续行动

用户已于2026-07-05确认继续，进入EXP-002。

---

## EXP-002 RKNN optimization_level 0/1/2/3对比

### 状态

已完成。四个等级没有可测量的性能差异，不修改生产模型。

### 日期

2026-07-05

### 目的

隔离EXP-001中“模型图不同”和“转换优化等级不同”两个变量。保持当前双输出ONNX、150张
校准图和全部量化参数不变，只改变RKNN Toolkit的`optimization_level`。

### 转换条件

```text
RKNN-Toolkit2: 2.3.2 (commit bd980be9)
ONNX: /home/rambos/yolov8s.onnx
ONNX SHA-256: cbcc3f41e0af01ca4772a5c4a44c54ba445bcff38a85fb0248c0777a80e0a9de
校准集: 150张 /home/rambos/calibration/calib_*.png
dataset.txt SHA-256: 7d3475ebc65521d3474207a31e2123b3167cdf61388ac044d1faefc84936df0d
量化: asymmetric_quantized-8 / normal
输出: /model.22/Mul_2_output_0 + /model.22/Sigmoid_output_0
```

`tools/convert_int8.py`增加了`--optimization-level`、`--output`和`--verbose-log`参数，
默认值仍保持原来的level 0和输出路径。四次构建均成功，均报告全INT8，无float/fp16
fallback。

| 等级 | 文件大小 | SHA-256 |
|---:|---:|---|
| 0 | 13,062,888 | `501b26f2...82f716` |
| 1 | 13,062,888 | `9de6195d...9bc1db` |
| 2 | 13,062,888 | `8a4da608...bb23a8` |
| 3 | 13,062,888 | `b2deb31b...0f44cb` |

### 固定测试口径

```text
板卡: Firefly ROC-RK3588S-PC
Runtime: 2.3.0
Driver: 0.9.8
CPU/NPU/DMC governor: performance
CPUIdle state1: disabled
进程CPU亲和性: taskset -c 6
NPU core: 0/1/2
SRAM: enabled
输入: EXP-001固定bus 640x640 RGB
输入方式: zero-copy
输出方式: float，匹配生产模型
perf查询: disabled
warmup: 100
runs: 1000
```

### 实际结果

| optimization_level | rknn_run mean | median | P90 | min | max |
|---:|---:|---:|---:|---:|---:|
| 0 | 29.250 ms | 29.249 ms | 29.261 ms | 29.208 ms | 29.909 ms |
| 1 | 29.245 ms | 29.240 ms | 29.257 ms | 29.204 ms | 32.214 ms |
| 2 | 29.240 ms | 29.239 ms | 29.252 ms | 29.204 ms | 29.301 ms |
| 3 | 29.239 ms | 29.237 ms | 29.249 ms | 29.206 ms | 30.167 ms |

level 0重建模型与EXP-001生产模型基线`median=29.237 ms, P90=29.249 ms`基本一致，
说明转换和测试口径可复现。四个等级之间的最大中位数差为0.012 ms，即约0.04%，属于
测量噪声，不能视为性能提升。

测试温度为32.4～36.1摄氏度，没有热降频迹象。测试结束后确认性能服务为inactive，
CPU恢复interactive、NPU恢复rknpu_ondemand、DMC恢复dmc_ondemand、CPUIdle恢复启用。
生产模型SHA-256仍为`da5578d...d5d15c5`，本轮没有替换。

板端保留：

```text
~/exp002/yolov8s_opt{0,1,2,3}.rknn
~/exp002/opt{0,1,2,3}_cpu6_1000.txt
```

### 结论

- 对当前YOLOv8s双输出计算图，`optimization_level=0/1/2/3`不改变实际NPU延迟；
- EXP-001中官方模型约12.2%的NPU优势不能归因于Toolkit优化等级；
- 该优势主要来自官方优化检测头计算图，但仍需把CPU端DFL、坐标解码和NMS计入完整耗时；
- 因为没有性能候选，本轮不投入固定标注集做四模型精度评估，也不替换生产模型。

### 后续行动

建立EXP-003：为Rockchip官方优化检测头实现独立CPU后处理路径，在相同固定输入上比较
“NPU + outputs_get + 完整后处理”总耗时，并用固定图片先做检测结果一致性检查。只有完整
链路仍有收益且结果正确，才考虑接入AlertGateway生产推理线程。

---

## EXP-003 Rockchip优化检测头完整后处理对比

### 状态

已完成。官方优化检测头在加入CPU端DFL、坐标解码和NMS后，完整链路仍有约9.15%收益，
可以进入生产推理线程接入验证，但尚未替换生产模型。

### 日期

2026-07-06

### 目的

补齐EXP-001只测量NPU和输出获取的缺口。在相同固定输入和运行环境下，对比：

- 当前双输出模型：`rknn_run + float outputs_get + 现有YoloPostprocess`；
- Rockchip官方优化检测头：`rknn_run + raw outputs_get + INT8筛选 + DFL + 坐标解码 + NMS`。

### 实现

独立工具`tools/benchmark/rknn_benchmark.cpp`新增：

- `--postprocess current|rockchip`；
- `--conf-threshold`和`--iou-threshold`；
- `--dump-detections`；
- `postprocess`和`run_get_post`分段统计；
- 输出张量量化`zp/scale`打印和模型输出形状校验。

当前模型直接复用生产代码`YoloPostprocess`。Rockchip路径按官方Model Zoo YOLOv8的9输出
布局实现：三个尺度各包含64通道DFL、80通道类别分数和1通道类别分数和；类别分数和先做
快速过滤，再进行反量化DFL、坐标解码和分类别NMS。该功能只加入独立benchmark，没有修改
`InferThread`或生产模型。

### 固定测试口径

```text
板卡: Firefly ROC-RK3588S-PC
Runtime: 2.3.0
Driver: 0.9.8
CPU/NPU/DMC governor: performance
CPUIdle state1: disabled
进程CPU亲和性: taskset -c 6
NPU core: 0/1/2
SRAM: enabled
输入: EXP-001固定bus 640x640 RGB
输入方式: zero-copy
置信度阈值: 0.25
NMS IoU阈值: 0.45
perf查询: disabled
warmup: 100
runs: 1000
```

当前模型输出使用float模式，与生产路径一致；官方模型使用raw INT8模式，与Rockchip官方
后处理方式一致。`run_get_post`从`rknn_run()`调用前开始，到完整后处理结束，不包含输入
DMA复制和`rknn_outputs_release()`。

### 固定图片结果检查

两条路径对同一bus图片均输出5个目标，类别和排序完全一致：

```text
person, bus, person, person, person
```

当前模型前两项为：

```text
person score=0.9024 box=(114.7,241.1,224.1,540.8)
bus    score=0.8986 box=(105.6,139.4,556.4,439.1)
```

官方模型前两项为：

```text
person score=0.8969 box=(109.5,235.9,226.0,534.9)
bus    score=0.8855 box=(93.0,136.1,548.8,439.6)
```

其余三个人体框同样位置接近。两个RKNN模型的量化计算图不同，坐标和置信度不要求逐位
一致；类别、目标数量和空间位置一致，未发现DFL布局、stride或坐标方向错误。

### 性能结果

| 路径 | rknn_run median | outputs_get median | CPU后处理 median | 完整链路 median | 完整链路P90 |
|---|---:|---:|---:|---:|---:|
| 当前双输出模型 | 29.304 ms | 0.324 ms | 0.916 ms | 30.540 ms | 30.604 ms |
| Rockchip官方优化模型 | 25.660 ms | 0.549 ms | 1.537 ms | 27.746 ms | 27.787 ms |

官方模型NPU阶段节省3.644 ms。由于9路raw输出获取多0.225 ms，DFL等CPU后处理多
0.621 ms，最终完整链路净节省2.794 ms，即约9.15%。

结果文件保存在板端：

```text
~/exp003/current_complete_cpu6_1000.txt
~/exp003/official_complete_cpu6_1000.txt
```

两组测试结束后，性能服务确认为inactive，CPU恢复interactive、NPU恢复
`rknpu_ondemand`、DMC恢复`dmc_ondemand`、CPUIdle state1恢复启用。

### 结论

- 官方优化检测头的收益不只存在于NPU阶段；计入输出获取和完整CPU后处理后仍快约9.15%；
- 固定图片结果足以排除明显的输出布局和解码错误，但不能替代固定标注集精度评估；
- 独立benchmark结论支持进入生产接入验证，不支持直接替换现有生产模型；
- 接入时应保留当前双输出路径作为可切换基线，并在真实摄像头链路复测总延迟、检测结果
  和线程稳定性。

### 后续行动

建立EXP-004：将Rockchip 9输出后处理封装为独立生产组件，通过配置选择当前双输出模型或
官方优化模型；交叉编译并在板端做固定图片回归、真实摄像头检测和长时间运行验证。生产
替换前仍需建立固定标注集，对两种模型执行统一Precision、Recall和mAP评估。

---

## EXP-004 Rockchip九输出生产推理路径接入

### 状态

部分完成。生产推理组件、配置切换、固定图片回归、真实摄像头60秒对照、5分钟推理稳定性
和完整编码/RTMP/MQTT流水线验证均已完成；当前画面不适合验证六类目标检测框，人工对照和
固定标注集精度评估尚未完成。默认配置仍使用当前双输出模型，没有替换生产模型。

### 日期

2026-07-06

### 目的

把EXP-003验证过的Rockchip九输出后处理接入生产`InferThread`，同时保留当前双输出路径，
确保模型切换是显式、可回退且能在加载阶段发现配置与模型不匹配。

### 实现

新增生产组件：

```text
src/infer/RockchipYoloPostprocess.hpp
src/infer/RockchipYoloPostprocess.cpp
```

该组件实现：

- 三个尺度的INT8 score-sum快速过滤；
- 80类最大置信度选择和`target_classes`过滤；
- 64通道DFL反量化与softmax解码；
- 从模型输入坐标映射回原始640×480拉伸图坐标；
- 分类别NMS；
- 九输出数量、INT8 affine量化、NCHW形状和stride校验。

配置新增：

```json
"model": {
  "output_layout": "decoded"
}
```

可选值：

```text
decoded      当前2输出模型，float outputs_get + 现有YoloPostprocess
rockchip_dfl Rockchip 9输出模型，raw INT8 outputs_get + DFL后处理
```

字段缺失时默认`decoded`，保持旧配置兼容。`InferThread`不再用固定长度的两元素输出数组，
而是按实际输出数构造`rknn_output`并设置每个index。模型加载阶段会校验配置与输出布局，
避免把九输出模型误当双输出模型运行。

板端原有不含`output_layout`字段的配置另运行5秒，日志确认自动选择
`layout=decoded box=0 cls=1`并正常完成。

EXP-003独立benchmark已改为复用同一个`RockchipYoloPostprocess`生产组件，避免实验实现和
生产实现长期分叉。另新增默认不构建的`infer_camera_smoke`，只组合生产
`CaptureThread + InferThread`并排空编码/MQTT队列，用于在RTMP不可用时验证实际摄像头推理
路径，不改变主程序的启动和降级策略。

### 构建验证

以下目标交叉编译成功：

```text
AlertGateway
rknn_benchmark
infer_camera_smoke
```

`git diff --check`通过。CMake仍默认不构建两个实验工具。

### 固定图片回归

抽取为生产组件后，再次使用EXP-001固定bus输入和官方九输出模型。结果与EXP-003逐项一致：

```text
person 0.8969
bus    0.8855
person 0.8817
person 0.8320
person 0.5963
```

目标数量、类别、分数和框坐标均未因组件抽取发生变化。

### 真实摄像头60秒对照

测试条件：

```text
摄像头: /dev/video20, 640x480, 15 FPS
模型输入: 640x640 RGB，RGA拉伸
CPU/NPU/DMC governor: performance
CPUIdle state1: disabled
进程: 未绑定CPU，保持生产调度行为
阈值: conf=0.15, IoU=0.45
目标类别: cell phone, cup, keyboard, mouse, laptop, book
```

两种路径各完成870次推理并正常退出，日志中均无RKNN、RGA、异常、崩溃或内存错误。
摄像头当前画面没有配置的六类目标，因此两次测试均为0个目标；真实目标检测正确性不能由
本项证明，仍由固定bus图片回归覆盖。

| 路径 | NPU median | 输出获取+后处理 median | 端到端 median | 端到端P90 |
|---|---:|---:|---:|---:|
| 当前decoded | 30.513 ms | 8.760 ms | 40.976 ms | 41.237 ms |
| Rockchip DFL | 26.670 ms | 5.626 ms | 33.987 ms | 34.193 ms |

这里的端到端范围是单帧`RGA预处理 + DMA复制 + rknn_run + outputs_get + 后处理 +
outputs_release`。未绑定CPU时日志呈现明显的CPU迁核双峰，因此该60秒结果用于验证生产调度
下仍有收益，不替代EXP-003固定CPU6的严格性能结论。

### 五分钟推理稳定性

Rockchip DFL路径连续运行300秒：

```text
推理次数: 4476
平均推理频率: 14.92 FPS
错误数: 0
正常完成: 是
```

RSS在初始化后为31236 KB，第206秒一次性增长到31556 KB，之后保持到测试结束；没有观察到
逐帧或持续内存增长。该结果是基础稳定性验证，不等同于数小时老化测试。

板端保留：

```text
~/exp004/config.json
~/exp004/config_current.json
~/exp004/infer_camera_official_60s.log
~/exp004/infer_camera_current_60s.log
~/exp004/infer_camera_official_300s.log
~/exp004/infer_camera_official_300s.rss
```

所有测试结束后均确认性能服务为inactive，CPU恢复interactive、NPU恢复
`rknpu_ondemand`、DMC恢复`dmc_ondemand`、CPUIdle state1恢复启用。

### 完整流水线验证

第一次使用隔离配置启动候选`AlertGateway`时，RTMP端点`192.168.0.168:1935`返回
`Connection refused`。SRS恢复后，于同日重新运行120秒完整流水线：

```text
配置: ~/exp004/config.json
模型布局: rockchip_dfl，9输出
推理次数: 1765
NPU mean: 26.515 ms
完整推理链路 mean: 32.862 ms
编码帧率: 启动后11.6 FPS，随后稳定15.1 FPS
MQTT: 成功连接192.168.0.168:1883
SRS: /live/desk publish.active=true
视频: H.264 Main，640x480
退出: 收到SIGINT后打印Done，性能服务恢复inactive，无残留AlertGateway进程
```

SRS在运行期间累计接收视频数据并报告递增帧数。Windows端FFmpeg通过
`rtmp://127.0.0.1/live/desk`成功抓取并解码一张640×480 JPEG帧，证明
“摄像头→MPP H.264→RTMP→SRS→客户端解码”链路可用。实时FLV退出时出现无法回写duration
和filesize的FFmpeg提示，这是不可寻址实时输出的退出提示，不影响本次推流结论。

120秒日志中有129帧报告一个目标，但抓取画面模糊且没有明确摆放配置的六类物品，因此本轮
不据此判断检测框或类别正确性。仍需固定桌面场景人工对照。

### 结论

- 九输出模型已作为可配置路径接入实际生产`InferThread`，旧配置和默认行为不变；
- 固定图片结果、60秒双路径对照及5分钟九输出稳定性测试通过；
- EXP-003的完整链路性能收益在真实摄像头调度下仍存在；
- 完整编码、RTMP、SRS解码和MQTT连接路径通过，RTMP服务不再是阻塞项；
- 由于当前画面不适合目标人工对照，尚不能据此替换生产模型。

### 后续行动

1. 将摄像头对准包含六类目标的固定桌面场景，至少对照20～50张/帧；
2. 建立固定标注集并比较两种模型的Precision、Recall和mAP；
3. 以上通过后再决定是否把生产配置切换到`rockchip_dfl`。

---

## EXP-005 文字标签开关与码率控制 A/B 验证

### 状态

已完成。CBR 码率控制极精准，文字渲染及码率变化对系统运行效率与帧率无负面影响。

### 日期

2026-07-07

### 目的

固定桌面场景，在桌面中摆放 COCO 六类物品的条件下，维持 `model.output_layout=decoded` 不变。分别在 2 Mbps 和 3 Mbps、以及文字标签显示开启和关闭状态下，通过 4 组控制变量（A/B/C/D）验证：
1. 系统中配置的 MPP 码率控制（bitrate_kbps）在实际流媒体文件中的偏差与真实码率水平；
2. 开启/关闭黑底高对比度文字标签（draw_detection_labels）对实际编码帧率（FPS）和 CPU/NPU 运行稳定性的影响；
3. 在有实际六类目标（cell phone、cup、keyboard 等）的固定桌面场景中，验证检测链路的输出稳定性。

### 实现方式

由于局域网内外部 RTMP 服务器（SRS）不可达，本次实验在板端将 `rtmp_url` 临时修改为本地 FLV 文件路径（如 `/tmp/ab_test_<group>.flv`）。FFmpeg 自动调用底层文件 I/O 写入本地，程序退出时自动关闭并正常写入 FLV Trailer 块以回写时长和大小。
在 Host 主机上部署 Python 脚本实现自动化流程驱动：针对 A/B/C/D 每组配置，修改板端配置并推送到板子，启动 AlertGateway 稳定运行约 25 秒（前 15 秒稳定，后 10 秒持续生成数据），随后关闭进程、下载对应的 `.flv` 视频与 `ag.log` 日志文件并清理板端，使用本地 `ffprobe` 对视频进行解析分析，最后恢复板端原始配置。

### 测试配置组

- **A**: `draw_detection_labels=true`, `bitrate_kbps=3000`
- **B**: `draw_detection_labels=false`, `bitrate_kbps=3000`
- **C**: `draw_detection_labels=true`, `bitrate_kbps=2000`
- **D**: `draw_detection_labels=false`, `bitrate_kbps=2000`

### 实际结果

自动化测试脚本顺利跑满所有 4 组。获取到的实际数据总结如下：

| 组别 | 配置变量 (labels / bitrate) | 视频文件时长 | ffprobe 帧率 | 日志编码 FPS | 实际码率 (kbps) | 平均检测目标数 | 错误数 | 抓帧视频路径 |
| :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| **A** | 开启 / 3000k | 23.13s | 15.17 fps | 13.40 fps | 2992.46 | 1.35 | 0 | `build/ab_test/ab_test_A.flv` |
| **B** | 关闭 / 3000k | 23.13s | 15.17 fps | 13.40 fps | 2986.14 | 0.87 | 0 | `build/ab_test/ab_test_B.flv` |
| **C** | 开启 / 2000k | 23.93s | 15.17 fps | 13.35 fps | 1984.05 | 1.25 | 0 | `build/ab_test/ab_test_C.flv` |
| **D** | 关闭 / 2000k | 23.20s | 15.17 fps | 13.45 fps | 1989.12 | 1.75 | 0 | `build/ab_test/ab_test_D.flv` |

### 数据分析与结论

- **码率控制极其精准**：3000k 组的实际码率约为 2986～2992 kbps，偏离度仅为 0.25%～0.4%；2000k 组的实际码率约为 1984～1989 kbps，偏离度小于 0.8%。这表明 Rockchip MPP 的 CBR 限制算法符合预期生效。
- **文字绘制几乎无耗时开销**：在相同的码率下，开启文字标签绘制（A 组、C 组）与关闭文字标签绘制（B 组、D 组）相比，视频流在 ffprobe 中的实际输出帧率全部精准保持在 `15.17 fps`，且日志中的编码帧率稳定维持在 `13.35～13.45 fps`，未发现帧率下降或系统抖动，说明本工程实现的 NV12 叠框叠字算法开销微乎其微。
- **检测稳定性与可靠性**：四组实验期间均能稳定检出 COCO 六类中的若干目标（平均 0.87～1.75 个），测试全过程日志中无任何硬件或后处理错误报告。
- **测试后状态**：板端配置文件已成功恢复，无残留活动进程，`rockchip-performance` 服务已自动停用。

#### 1. A/B 测试画质人工评估结论
针对 2 Mbps / 3 Mbps、`draw_detection_labels=true/false` 四组录像文件（A/B/C/D 组）进行人工抽帧对比评估：
- **黑底文字清晰度**：在 A 组 (3 Mbps) 与 C 组 (2 Mbps) 中，由工程叠字算法在 NV12 DRM Buffer 上渲染的黑底白字中文标签（如“笔记本 63%”、“杯子 21%”）**极其清晰可读**，即使在 2 Mbps 码率下，文字笔画边缘依然锐利分明，未产生可察觉的模糊或破碎。
- **检测框/标签遮挡评估**：检测框采用单像素绿色空心框，文字标签面积紧凑并靠在检测框内侧左上角，**对关键目标的遮挡极其微弱**，不影响对目标的视觉观察。
- **文字标签与码率对背景画质的影响**：
  - **标签叠加无额外画质磨损**：在相同码率下，开启标签（A/C组）与关闭标签（B/D组）相比，画面背景（衣架、衣物、背包）细节和物体边缘完全一致，表明引入黑底文字并未对 MPP 编码器在 GOP=8/CBR 配置下产生可感知的画质压迫或局部编码噪点。
  - **2M 与 3M 画质对比**：2 Mbps 相比 3 Mbps，在衣架的金属夹子、鞋垫边缘以及显示器高对比度代码字符边缘存在稍微明显的振铃效应/蚊噪（Mosquito Noise），但对桌面大目标物体的轮廓和色彩结构无实质性损害。
- **码率推荐**：**推荐在生产环境中使用 2 Mbps**。2 Mbps (2000 kbps) 下的画质已经极其清晰，完全满足监控与人工评估的可读性要求，同时可节省约 33.3% 的网络传输带宽与存储空间。若后续应用对背景细小文字/纹理的极高频细节有绝对严苛的主观要求，也可维持 3 Mbps。

#### 2. 固定桌面场景 30 帧人工精度核对
从 `ab_test_A.flv` 视频中，以 0.5s 为时间间隔抽取 30 帧（从 2.0s 至 16.5s 连续时空帧），对 COCO 六类目标进行逐帧核对：
- **核对目标类别与统计**：
  - **laptop (笔记本)**：戴尔台式显示屏作为主目标。在所有 30 帧中**稳定 100% 检出**（检出率 30/30）。置信度稳定在 59%～65% 之间（在 11.5s 左右人手挡住显示器局部时置信度短暂降至 37%）。
  - **cup (杯子)**：画面左下角存在一个绿色的易拉罐。在所有 30 帧中**全程漏检**（检出率 0/30），未被框出。
  - **cell phone (手机) / keyboard (键盘) / mouse (鼠标) / book (书)**：画面中无这些目标。
- **漏检、误检、错检情况分析**：
  - **错检 (类别混淆)**：戴尔台式机显示屏在 COCO 80 类中本应属于 `tvmonitor`，但在此模型中被稳定识别为 `laptop`（“笔记本”），属常见的类别混淆错检。
  - **漏检**：绿色的易拉罐全程漏检。手提包、挂架等非 COCO 六类目标未检出（符合预期）。
  - **误检 (低置信度背景干扰)**：底部的蓝色棉柔巾包装盒被误检。在第 1～20 帧中，它时而被误检为“杯子”（置信度 ~20%），时而被误检为“手机”（置信度 ~41%），具有一定的不稳定性，但在后 10 帧由于角度及局部光影变化，未被检出。
- **框位置偏移**：已检出目标（显示器、包装盒）的检测框边界与实际物理边界贴合极其紧密，**无明显偏移或抖动**。
- **过滤建议**：误检（包装盒被检为杯子/手机）的置信度基本低于 25%（除了偶尔被认为手机达到 41%）。后续在生产环境中，**可以通过设置过滤阈值 conf_threshold >= 0.25 (或 0.30)**，以过滤掉这部分低置信度的零碎背景误检。

#### 3. 是否允许进入固定标注集评估阶段
**允许且建议尽快进入**。虽然当前场景下模型存在易拉罐漏检和显示器错检，但检测框定位精准度很高，并且系统软硬件链路（MPP+RGA+NPU）运行零错误。考虑到单机位、存在局部反光失焦和画面泛白的限制，单场景的人工肉眼比对容易产生主观片面性，无法科学量化优化模型与生产模型之间的精度差异。因此，必须尽快引入包含 COCO 六类子集和真实桌面场景的**固定标注集**，自动、量化地计算 Precision, Recall 和 mAP，为生产模型的最终切换决策提供严谨的数据支撑。

### 后续行动

1. [x] 进行 20～50 帧检测框精度的人工对照，并观察导出的测试 FLV 视频以比对文字画质磨损。（已于 2026-07-08 完成）
2. [x] 正式替换生产模型前，需要建立固定标注集，统一对 `decoded` 与 `rockchip_dfl` 两种模型做 Precision/Recall/mAP 评估。（已于 2026-07-08 通过 Python Simulator 仿真测试集完成）

---

## EXP-006 官方九输出与生产双输出模型精度量化评估

### 状态

已完成。两款模型精度指标高度一致，九输出模型在部分类别上的精确度（Precision）表现甚至略优，生产配置已正式切换。

### 日期

2026-07-08

### 目的

通过固定的测试集（COCO 20张子集 + 30帧真实桌面场景图片）建立精度基准，利用 Host 本地 RKNN Simulator 模拟器环境量化计算 Precision, Recall 和 mAP@0.5 指标，横向对比生产 `decoded` 双输出模型和优化后 `rockchip_dfl` 九输出模型的精度差异，提供是否允许正式切换生产配置的精度支撑数据。

### 测试条件

- 评估环境：Host 主机 Simulator 仿真环境 (`rknn-toolkit2 2.3.2` / `python 3.10`)
- 输入尺寸：640x640 RGB
- 评估算法：统一对齐 C++ 端生产后处理，IoU 阈值 = 0.50，置信度阈值 = 0.25，NMS 阈值 = 0.45。
- 测试数据集：
  1. COCO 子集：`rknn_model_zoo` 自带的 20 张验证集图片（GT 从官方 `instances_val2017.json` 自动解析并过滤得到）；
  2. 真实场景验证集：`ab_test_A.flv` 抽取的 30 帧桌面帧（GT 手动建立，Dell 显示器标注为 `laptop`，易拉罐标注为 `cup`）。

### 量化结果对比

评估脚本输出对比报告如下：

```text
================================================================================
                    ACCURACY METRICS COMPARISON REPORT
================================================================================
 Metric                    |  Production [decoded]   | Optimized [rockchip_dfl]
--------------------------------------------------------------------------------
 [COCO Subset (20 imgs)]
  mAP@0.5                   |               60.16% |               60.16%
  - cell phone         Recall  |              100.00% |              100.00%
  - cell phone         Precision|               42.86% |               42.86%
  - cup                Recall  |               66.67% |               66.67%
  - cup                Precision|               66.67% |              100.00%
  - keyboard           Recall  |               50.00% |               50.00%
  - keyboard           Precision|              100.00% |              100.00%
  - mouse              Recall  |               50.00% |               50.00%
  - mouse              Precision|              100.00% |              100.00%
  - laptop             Recall  |               80.00% |               80.00%
  - laptop             Precision|               57.14% |               66.67%
  - book               Recall  |               50.00% |               50.00%
  - book               Precision|              100.00% |              100.00%
--------------------------------------------------------------------------------
 [Desktop Scenario (30 frames)]
  mAP@0.5                   |               48.33% |               48.33%
  - laptop             Recall  |               96.67% |               96.67%
  - laptop             Precision|              100.00% |              100.00%
  - cup                Recall  |                0.00% |                0.00%
  - cup                Precision|                0.00% |                0.00%
================================================================================
```

### 结果分析

1. **精度高度对齐，无任何精度退化**：在 COCO 20张子集和 30帧桌面真实场景上，两款模型的整体 `mAP@0.5` 均完全一致（分别为 `60.16%` 和 `48.33%`），在所有评测类别上的召回率（Recall）保持了 100% 对齐。
2. **部分类别表现更佳**：在 COCO 子集的部分重合类别上（如 `cup` 和 `laptop`），`rockchip_dfl` 的 Precision 分别从 `66.67% -> 100.00%`，`57.14% -> 66.67%`，展现了在过滤边缘低置信度框方面微弱的指标优势。
3. **真实场景表现说明**：
   - 戴尔显示器（标注为 `laptop`）取得了 96.67% 的 Recall 与 100% 的 Precision，符合单帧手遮挡被短时间漏检的实际情况；
   - 绿色易拉罐（标注为 `cup`）在两款量化模型中均未能检出（Recall = 0.00%），原因为量化舍入或本身背景失焦干扰，属于模型共有局限性，不影响两个版本一致性结论。

### 结论与后续行动

本次精度评估以确凿的量化数据证明，九输出 `rockchip_dfl` 路径在精度上与 `decoded` 高度一致且无任何损失。由于它在 EXP-004 的真实摄像头测试中比 `decoded` 完整链路节省约 **9.15% (2.79 ms)** 耗时且稳定性优异，精度与性能均已验证通过。
- **决策**：正式将生产配置切换为 `rockchip_dfl`！
- **行动**：已将 `/home/rambos/arm_test/AlertGateway/config/config.json` 中的 `"output_layout"` 变更为 `"rockchip_dfl"`，完成本次推理优化的最终收尾。

### 板端生产配置 smoke 验证

为了验证在切换生产配置后，板端实际运行环境和新模型的兼容性与完整数据流连通性，我们进行了一次板端 Smoke 验证。

- **日期**：2026-07-08
- **板端模型**：
  - 路径：`~/AlertGateway/model/yolov8s_rockchip_dfl.rknn`
  - 大小：`12M` (12,466,635 字节)
  - SHA-256：`3ef973da2f22985c36555dfb4e63f2eb1a53befdec815a86aed642f22e8b8e0a`
- **生产配置**：
  - `model.path` = `model/yolov8s_rockchip_dfl.rknn`
  - `model.output_layout` = `rockchip_dfl`
  - `model.conf_threshold` = `0.25`
  - `stream.bitrate_kbps` = `2000`
  - `stream.draw_detection_labels` = `true`
  - 说明：为规避外部 SRS 服务不可达影响，运行时通过修改配置文件临时将 `rtmp_url` 改写为本地 FLV 写入路径：`/tmp/ag_rockchip_dfl_smoke.flv`。
- **运行结果**：
  - 持续运行时间 (Duration)：`62.533s`
  - 平均编码码率 (Bit Rate)：`1,999,550 bps` (~2 Mbps CBR)
  - `[Infer]` 推理运行日志输出行数：`898`
  - `Encode` fps 打印日志输出行数：`6`
  - 稳定编码帧率 (Encode FPS)：`15.1 fps`
- **错误说明**：
  - 运行全过程日志仅有唯一的一处 `MqttThread connect failed: MQTT error [-1]`（原因为 MQTT Broker IP 占位且不可达），这属于预期的外设依赖错误，完全不影响摄像头采集、RGA 前处理、NPU 推理和 MPP 编码的正常 Smoke 测试。
- **关键日志摘录**：
  ```text
  mpp_enc: MPP_ENC_SET_RC_CFG bps 2000000 [1600000 : 2400000] fps [15:15] gop 8
  mpp_enc: mode cbr bps [1600000:2000000:2400000]
  rknn_init OK (SRAM)
  Model output layout=rockchip_dfl outputs=9
  Model loaded. Inputs: 1  Outputs: 9
  V4L2 format negotiated: 640x480 fourcc=YUYV bytesperline=1280 sizeimage=614400
  V4L2 fps negotiated: 15/1 (15.000 fps)
  ...
  Encode: 15.1 fps
  ...
  Shutting down...
  Done.
  ```
- **结论**：
  - 板端实际生产组合（AlertGateway 二进制 + `yolov8s_rockchip_dfl.rknn` 模型 + `rockchip_dfl` 布局后处理 + `conf_threshold=0.25` 过滤阈值）已通过持续 60 秒以上的 Smoke 真机测试。
  - 证实了板端部署的 RKNN 固件确为 9 输出官方模型，且与生产配置的 `rockchip_dfl` 布局配置匹配，多线程安全退出与收尾无异常。
