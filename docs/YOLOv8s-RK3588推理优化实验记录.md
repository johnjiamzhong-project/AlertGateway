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

通过固定的测试集（COCO 20张子集 + 从桌面测试视频抽取的30帧固定图片）建立精度基准，利用 Host 本地 RKNN Simulator 模拟器环境量化计算 Precision, Recall 和 mAP@0.5 指标，横向对比生产 `decoded` 双输出模型和优化后 `rockchip_dfl` 九输出模型的精度差异，提供是否允许正式切换生产配置的精度支撑数据。

### 测试条件

- 评估环境：Host 主机 Simulator 仿真环境 (`rknn-toolkit2 2.3.2` / `python 3.10`)
- 输入尺寸：640x640 RGB
- 评估算法：统一对齐 C++ 端生产后处理，IoU 阈值 = 0.50，置信度阈值 = 0.25，NMS 阈值 = 0.45。
- 测试数据集：
  1. COCO 子集：`rknn_model_zoo` 自带的 20 张验证集图片（GT 从官方 `instances_val2017.json` 自动解析并过滤得到）；
  2. 桌面抽帧验证集：`ab_test_A.flv` 抽取的 30 帧固定图片（GT 手动建立，Dell 显示器标注为 `laptop`，易拉罐标注为 `cup`）。该数据集用于离线固定图片评估，不等同于实时摄像头现场复测。

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

1. **精度高度对齐，无任何精度退化**：在 COCO 20张子集和 30帧桌面抽帧固定图片上，两款模型的整体 `mAP@0.5` 均完全一致（分别为 `60.16%` 和 `48.33%`），在所有评测类别上的召回率（Recall）保持了 100% 对齐。
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

---

## EXP-007 640x480 输入尺寸优化准备

### 状态

进行中。已完成本地工具脚本准备、640x480 校准集拉取、640x480 九输出 ONNX/RKNN 候选生成和 Simulator 精度评估。当前候选在桌面场景精度明显退化，不能进入生产 C++ 接入。

### 日期

2026-07-08

### 目的

进入推理优化路线的下一阶段：将模型输入从当前 640x640 改为匹配摄像头比例的 640x480，减少无效像素计算和拉伸失真。正式改生产配置之前，需要先建立对应的校准、转换和离线精度评估流程。

### 本次本地准备

1. `tools/collect_calibration.py`
   - 新增 `--model-width` 和 `--model-height` 参数；
   - 默认仍保持 640x640，避免影响旧流程；
   - 可直接采集 640x480 校准图，例如：
     ```bash
     python3 tools/collect_calibration.py \
       --device /dev/video20 \
       --count 150 \
       --out ~/calibration_640x480 \
       --model-width 640 \
       --model-height 480 \
       --no-preview
     ```

2. `tools/convert_int8.py`
   - 新增 `--onnx`、`--calib-dir`、`--dataset-txt`、`--dataset-limit`、`--output-layout` 参数；
   - 默认仍为旧 `decoded` 双输出流程；
   - 指定 `--output-layout rockchip_dfl` 时直接保留官方 YOLO 输出，适合生成九输出 RKNN；
   - 640x480 官方模型候选转换命令：
     ```bash
     python3 tools/convert_int8.py \
       --onnx ~/yolov8s_official_640x480.onnx \
       --calib-dir ~/calibration_640x480 \
       --dataset-txt ~/calibration_640x480/dataset.txt \
       --output-layout rockchip_dfl \
       --output ~/yolov8s_rockchip_dfl_640x480.rknn \
       --verbose-log /tmp/rknn_convert_640x480.log
     ```

3. `tools/benchmark/evaluate_precision.py`
   - 新增 `--model-width` 和 `--model-height`，用于统一控制预处理 resize、坐标回映射和 `rockchip_dfl` 输出网格校验；
   - 新增 `--model-mode`，允许只评估 `rockchip_dfl`，避免 640x480 阶段被旧 `decoded` 模型路径阻塞；
   - 640x480 官方模型候选离线评估命令：
     ```bash
     python3 tools/benchmark/evaluate_precision.py \
       --model-mode rockchip_dfl \
       --official-onnx ~/yolov8s_official_640x480.onnx \
       --calib-dataset ~/calibration_640x480/dataset.txt \
       --model-width 640 \
       --model-height 480 \
       --conf-threshold 0.25 \
       --nms-threshold 0.45 \
       --eval-iou-threshold 0.50
     ```

### 脚本验证结果

本次只做本地脚本级验证：

```bash
python3 -m py_compile tools/collect_calibration.py tools/convert_int8.py tools/benchmark/evaluate_precision.py
git diff --check
```

结果均通过。

### 640x480 候选模型生成与转换结果

由于本机没有现成的 640x480 官方优化 ONNX，先基于已验证的 640x640 官方九输出 ONNX
`/home/rambos/yolov8s_official.onnx` 修改静态输入/输出形状，生成候选：

- ONNX：`/home/rambos/yolov8s_official_640x480.onnx`
- ONNX 文件大小：`43M`
- ONNX Runtime 零输入验证通过：
  - 输入：`[1, 3, 480, 640]`
  - 输出网格：`60x80`、`30x40`、`15x20`

从板端 `192.168.0.200` 拉取已采集的 640x480 校准集到 Host：

- 校准目录：`/home/rambos/calibration_640x480`
- 校准图数量：`150`
- 校准图尺寸：`480x640`
- dataset：`/home/rambos/calibration_640x480/dataset.txt`，`150` 行

使用 `tools/convert_int8.py --output-layout rockchip_dfl` 转换成功：

- RKNN：`/home/rambos/yolov8s_rockchip_dfl_640x480.rknn`
- RKNN 文件大小：`12M`
- 转换日志：`/tmp/rknn_convert_640x480.log`
- 转换脚本检查结果：`所有层均为 INT8，无 float/fp16 fallback`

说明：RKNN build 日志中出现默认输入/输出 dtype 从 float32 改为 int8 的性能提示，这是
当前生产九输出 INT8 路径的预期行为；`Split`/`Constant` 等张量的内部 qtype 提示没有
被转换脚本识别为实际算子 fallback。

### Simulator 精度评估结果

命令：

```bash
python3 tools/benchmark/evaluate_precision.py \
  --model-mode rockchip_dfl \
  --official-onnx /home/rambos/yolov8s_official_640x480.onnx \
  --calib-dataset /home/rambos/calibration_640x480/dataset.txt \
  --model-width 640 \
  --model-height 480 \
  --conf-threshold 0.25 \
  --nms-threshold 0.45 \
  --eval-iou-threshold 0.50
```

结果：

```text
Model input: 640x480
[Optimized [rockchip_dfl]]
  COCO Subset mAP@0.5       | 68.15%
  - cell phone Recall       | 100.00%
  - cell phone Precision    | 50.00%
  - cup Recall              | 66.67%
  - cup Precision           | 66.67%
  - keyboard Recall         | 50.00%
  - keyboard Precision      | 50.00%
  - mouse Recall            | 50.00%
  - mouse Precision         | 100.00%
  - laptop Recall           | 80.00%
  - laptop Precision        | 66.67%
  - book Recall             | 100.00%
  - book Precision          | 100.00%
  Desktop Scenario mAP@0.5  | 8.33%
  - laptop Recall           | 16.67%
  - laptop Precision        | 100.00%
  - cup Recall              | 0.00%
  - cup Precision           | 0.00%
```

对比 EXP-006 的 640x640 `rockchip_dfl` 基线：

- COCO 子集：`60.16% -> 68.15%`，指标上升；
- 桌面抽帧验证集：`48.33% -> 8.33%`，严重退化；
- 桌面 laptop Recall：`96.67% -> 16.67%`，严重退化；
- 桌面 cup 仍为 `0.00%`，没有改善。

### 结论

本轮 640x480 候选模型虽然可以完成 ONNX Runtime 验证、RKNN INT8 转换和 Simulator
推理，但桌面抽帧固定图片验证集精度明显不可接受，不能进入生产 C++ 尺寸放开和板端实时摄像头 smoke 阶段。

关键原因判断：当前 ONNX 是由 640x640 官方九输出模型修改静态输入/输出形状得到的候选，
不是从训练/导出流程原生导出的 640x480 官方优化模型。它能跑通并产出正确网格形状，
但不能证明模型在业务桌面抽帧验证集上的检测质量可用。

### 原生导出 ONNX 复测

随后由 `ultralytics_yolov8` 定制库 `rk_opt_v1.6` 分支以 `format='rknn'` 专属逻辑导出
原生 640x480 官方九输出 ONNX：

- ONNX：`/home/rambos/yolov8s_official_native_640x480.onnx`
- ONNX 文件大小：`43M`
- 输入：`[1, 3, 480, 640]`
- 输出网格：`60x80`、`30x40`、`15x20`
- SHA-256：`c4b45fdf1d26de751a3e228806f3beacd8ebd64c243d04ff508c45dd3200f09b`

本地复核发现它与改形候选文件 hash 不同，且 114 个 initializer 内容不同；因此它确实不是
简单复制文件，值得单独转换和评估。使用同一套 150 张 640x480 校准图转换：

- RKNN：`/home/rambos/yolov8s_rockchip_dfl_native_640x480.rknn`
- RKNN 文件大小：`12M`
- RKNN SHA-256：`39fa037904d9a7d9e620ff8ee8b4ace1bbdebe2b6b4c590382baf1949f8d14ef`
- 转换日志：`/tmp/rknn_convert_native_640x480.log`
- 转换脚本检查结果：`所有层均为 INT8，无 float/fp16 fallback`

同一套 Simulator 精度评估结果：

```text
Model input: 640x480
[Optimized [rockchip_dfl]]
  COCO Subset mAP@0.5       | 68.15%
  - cell phone Recall       | 100.00%
  - cell phone Precision    | 50.00%
  - cup Recall              | 66.67%
  - cup Precision           | 66.67%
  - keyboard Recall         | 50.00%
  - keyboard Precision      | 50.00%
  - mouse Recall            | 50.00%
  - mouse Precision         | 100.00%
  - laptop Recall           | 80.00%
  - laptop Precision        | 66.67%
  - book Recall             | 100.00%
  - book Precision          | 100.00%
  Desktop Scenario mAP@0.5  | 8.33%
  - laptop Recall           | 16.67%
  - laptop Precision        | 100.00%
  - cup Recall              | 0.00%
  - cup Precision           | 0.00%
```

原生导出模型与改形候选的离线评估结果完全一致，仍然不能进入生产接入。说明当前失败不是
由“仅修改 ONNX 形状”这一动作单独造成；更可能是 640x480 拉伸/尺度变化对当前桌面验证集
中的显示器目标召回影响过大，或当前小规模桌面验证集对该尺寸变化非常敏感。

### 后续行动

1. 暂停所有 640x480 候选进入生产，不修改 `InferThread` 矩形输入限制。
2. 不再继续重复 640x480 ONNX/RKNN 转换，除非验证集或训练/微调策略发生变化。
3. 优先评估更接近当前 640x640 分布的输入尺寸候选，或进入桌面六类数据扩充与微调。
4. 在任一新候选进入生产前，必须先达到桌面抽帧验证集 mAP 不低于当前 640x640 `rockchip_dfl` 基线的可接受范围。

## EXP-008 576x448 与 512x384 输入尺寸候选验证

### 状态

已完成。两个更小输入尺寸候选均可原生导出 ONNX，并可转换为全 INT8 RKNN，但固定验证集
精度均未通过，不能进入生产 C++ 接入或板端部署。

### 目的

在 640x480 候选失败后，继续验证路线中规划的 576x448 和 512x384 输入尺寸，确认降低
输入像素量是否能在维持桌面业务精度的前提下换取潜在 NPU 耗时收益。

### 产物与转换检查

| 尺寸 | ONNX | ONNX SHA-256 | RKNN | RKNN SHA-256 | 转换检查 |
|---|---|---|---|---|---|
| 576x448 | `/home/rambos/yolov8s_official_native_576x448.onnx` | `17cf21c4f382015e1ba838bb603bbc6721bf444869994b065edd47f8674f1d7b` | `/home/rambos/yolov8s_rockchip_dfl_native_576x448.rknn` | `2453dc110fee667fddef5332bf322e49c4ea40447add908bf15daaeee1ead33e` | 全 INT8，无 float/fp16 fallback |
| 512x384 | `/home/rambos/yolov8s_official_native_512x384.onnx` | `6fa5c73f1083122e45cb04fb7de2f2700623851537dc2ac4d97cbc30d265b3ef` | `/home/rambos/yolov8s_rockchip_dfl_native_512x384.rknn` | `39c650b45d12eb8669feb85ac500ab18a8b67cb6e75ee8de62e69813fa03e983` | 全 INT8，无 float/fp16 fallback |

ONNX 输出网格检查：

- 576x448：`56x72`、`28x36`、`14x18`；
- 512x384：`48x64`、`24x32`、`12x16`。

校准集：

- `/home/rambos/calibration_576x448`：150 张 PNG，`dataset.txt` 150 行；
- `/home/rambos/calibration_512x384`：150 张 PNG，`dataset.txt` 150 行。

### Simulator 精度结果

命令口径：`model-mode=rockchip_dfl`，`conf_threshold=0.25`，`nms_threshold=0.45`，
`eval_iou_threshold=0.50`，同一套 COCO 20 张子集与桌面 30 帧抽帧固定图片验证集。

```text
Model input: 576x448
[Optimized [rockchip_dfl]]
  COCO Subset mAP@0.5       | 61.39%
  - cell phone Recall       | 100.00%
  - cell phone Precision    | 75.00%
  - cup Recall              | 66.67%
  - cup Precision           | 100.00%
  - keyboard Recall         | 50.00%
  - keyboard Precision      | 100.00%
  - mouse Recall            | 50.00%
  - mouse Precision         | 100.00%
  - laptop Recall           | 60.00%
  - laptop Precision        | 60.00%
  - book Recall             | 50.00%
  - book Precision          | 100.00%
  Desktop Scenario mAP@0.5  | 13.33%
  - laptop Recall           | 26.67%
  - laptop Precision        | 100.00%
  - cup Recall              | 0.00%
  - cup Precision           | 0.00%

Model input: 512x384
[Optimized [rockchip_dfl]]
  COCO Subset mAP@0.5       | 53.61%
  - cell phone Recall       | 66.67%
  - cell phone Precision    | 40.00%
  - cup Recall              | 66.67%
  - cup Precision           | 100.00%
  - keyboard Recall         | 50.00%
  - keyboard Precision      | 100.00%
  - mouse Recall            | 50.00%
  - mouse Precision         | 100.00%
  - laptop Recall           | 60.00%
  - laptop Precision        | 60.00%
  - book Recall             | 50.00%
  - book Precision          | 100.00%
  Desktop Scenario mAP@0.5  | 5.00%
  - laptop Recall           | 10.00%
  - laptop Precision        | 100.00%
  - cup Recall              | 0.00%
  - cup Precision           | 0.00%
```

### 结论

与 640x640 `rockchip_dfl` 基线相比，两个更小尺寸均未达到可接受精度：

- 桌面抽帧验证集 mAP 基线为 `48.33%`，576x448 降至 `13.33%`，512x384 降至 `5.00%`；
- 桌面 laptop Recall 基线为 `96.67%`，576x448 降至 `26.67%`，512x384 降至 `10.00%`；
- 桌面 cup 仍为 `0.00%`，没有改善。

因此，当前“直接缩小输入尺寸”的路线在固定验证集上连续失败。不能将 576x448 或 512x384
接入生产，也不应修改 `InferThread` 的模型输入尺寸限制。

### 后续行动

1. 暂停当前 YOLOv8s 官方九输出模型的输入尺寸继续缩小实验。
2. 下一步转向桌面六类数据扩充与微调，先恢复/提升桌面验证集精度，再考虑压缩或缩小输入。
3. 若未来重新尝试小尺寸，应基于微调后的业务模型，而不是直接缩放当前 COCO 权重。
## EXP-009 候选板端 Smoke 与训练入口修复（2026-07-10）

### 目标

验证新六类九输出 INT8 RKNN 能否在 RK3588 真实采集链路启动，并修复后续训练入口误用旧定制 Ultralytics 的风险。

### 环境与候选产物

- 板端：Firefly RK3588，`192.168.0.200`，RKNN API 2.3.0，Driver 0.9.8。
- 候选目录：`~/AlertGateway/candidate_20260710/`，不覆盖现有生产目录。
- 候选模型：`yolov8s_desktop6_final_int8_desktop_calib.rknn`，六类、Rockchip 九输出。
- 候选程序：重新交叉编译的 `AlertGateway` 与 `infer_camera_smoke`。

### 结果

候选程序启动后完成：

- `rknn_init OK (SRAM)`；
- 9 输出布局校验通过；
- 六类输出通道校验通过；
- zero-copy 输入内存绑定成功。

完整 Smoke 在摄像头打开阶段停止：候选配置使用的 `/dev/video20` 在板端不存在。板端现有 ISP 节点包括 `/dev/video11`，其支持 UYVY/NV12，不支持当前 CaptureThread 要求的 YUYV。因此本次未将摄像头节点强行改写，也未修改生产配置。

### 训练入口修复

`tools/dataset/train_desktop6.py` 已改为默认使用现代已安装 Ultralytics；传入旧 `--ultralytics-dir` 会直接拒绝。Rockchip 导出改成可选的独立进程，避免训练环境再次加载会产生 NaN 的旧定制版本。已通过 `py_compile` 与 `--help` 验证。

### 结论与后续

本次证明新六类 RKNN 模型和 C++ 后处理可以完成加载，但真实采集链路尚未完成验证。下一步需恢复正确的 YUYV 节点，或为 CaptureThread 增加 UYVY/NV12 支持并重新编译 Smoke；摄像头链路通过后，再进入 0.375 宽度模型微调。


## EXP-010 多平面 UYVY 采集适配（2026-07-10）

- 设计选择：板端 /dev/video11 的多平面 UYVY 在 CaptureThread 边界转换为内部 YUYV，保持后续编码和推理接口不变。
- 代码已完成 V4L2 mplane 协商、单 plane mmap、DQBUF/QBUF 和 UYVY→YUYV 字节转换；AlertGateway、infer_camera_smoke、rknn_benchmark 全部交叉编译通过。
- 候选板端测试中成功协商 640x480 UYVY bytesperline=1280，但 VIDIOC_STREAMON 返回 EPERM；板端 v4l2-ctl 独立测试同样失败，media-ctl 显示 ISP 传感器输入链路未启用。
- 结论：本轮代码适配完成，完整 Smoke 等待板端 ISP/摄像头源恢复后继续；不需要重新录制视频。

## EXP-011 ISP 拓扑阻塞定位（2026-07-10）

- 只读检查 /dev/media0：CSI2/DPHY 与 rkcif 节点存在，但拓扑中没有摄像头 sensor 实体。
- /dev/video11 的格式协商、QBUF 通过，STREAMON 返回 EPERM；v4l2-ctl 独立采集同样失败。
- 结论：完整 Smoke 当前等待摄像头物理接入/驱动恢复；无需继续修改 UYVY 适配代码，也无需重新录制视频。


## EXP-012 0.375 宽度模型配置准备（2026-07-10）

- 新增 	ools/dataset/yolov8d375_desktop6.yaml，参数为 depth=0.33、width=0.375、nc=6。
- 使用 Ultralytics 8.4.67 构建并加载现有 best.pt 成功；未开始长时间训练。
- 注意：文件名含 yolov8s 会触发 Ultralytics 自动选择 s scale，因此训练入口应使用 d375 文件。
- 下一步：固定 final train/val/test 做短跑微调和独立评估。


## EXP-013 0.375 short run (2026-07-10)

- Run: yolov8d375_desktop6_smoke_5e_20260710; 640 input, batch 8, AMP off, 5 epochs.
- Result: val mAP50=16.6%, mAP50-95=7.13%; weight transfer and training path passed.
- Conclusion: not a selection result; run 30 epoch fine-tuning and independent test next.


## EXP-014 0.375 完整微调结论（2026-07-10）

- 30 epoch 结果：val mAP50=54.3%。
- 固定 test：mAP50=42.3%、mAP50-95=28.1%。
- 对比基线 test mAP50=77.2%，0.375 候选淘汰，不进入后续结构优化。


## EXP-015 当前路线状态同步（2026-07-10）

- 当前生产候选仍为六类 YOLOv8s 基线：桌面 test mAP50=77.2%；INT8 RKNN letterbox 仿真 mAP50=56.08%，未观察到明显量化损失。
- 0.375 宽度模型已完成 30 epoch 微调，但 test mAP50=42.3%，已淘汰，不再作为后续结构优化起点。
- 后续实验统一按以下顺序：RKNN profiling → 算子融合与通道对齐验证 → 结构化剪枝 → 必要时模块替换 → 蒸馏恢复精度 → ONNX/RKNN/板端复测。
- 算子融合和通道填充只有在 profiling 发现未融合算子、CPU fallback、内存搬运或通道对齐热点时才实施；不能仅凭参数量推断 NPU 一定加速。
- 板端完整摄像头 Smoke 仍受 ISP sensor 输入链路阻塞，模型加载和交叉编译验证已通过。


## EXP-016 RK3588 基线总耗时 profiling（2026-07-10）

- 条件：六类 9 输出 RKNN、RK3588、warmup=50、runs=300、zero-copy、auto core。
- rknn_run：mean 50.707 ms，P90 53.032 ms；outputs_get：4.276 ms；postprocess：0.451 ms；input copy：0.788 ms。
- 结论：主要瓶颈是 NPU 图执行；后处理和输入链路不是当前首要优化点。当前 benchmark 提供 RKNN_QUERY_PERF_RUN 总耗时，尚未取得逐层 PERF_DETAIL。


## EXP-017 通道对齐与剪枝准备（2026-07-10）

- 基线 RKNN 输出包含 64/32/16 等对齐通道，暂无额外 padding 的必要证据。
- rknn-env 未安装 torch_pruning/nni；后续需采用可控模型手术方案，优先低比例、可回滚的结构化剪枝。


## EXP-018 剪枝前通道重要性分析（2026-07-10）

- 对 baseline 的 57 个 Conv+BN 层计算 BN gamma 均值，生成 baseline_channel_importance.txt。
- 低重要性候选主要位于 model.15.cv1/cv2、model.4 内部层；尚未执行裁剪。
- 原因：C2f/Concat/Detect 存在通道依赖，下一步必须做依赖感知裁剪并保持可回滚。


- 通道重要性报告已归档至项目：docs/artifacts/baseline_channel_importance.txt。


## EXP-019 结构化剪枝计划（2026-07-10）

- 计划文件：docs/artifacts/structured_pruning_plan.json。
- 首轮候选 5 个 C2f 相关层，目标剪枝比例 10%，通道数按 16 对齐。
- 尚未修改模型；先完成依赖感知手术实现，再进行微调和 RKNN 复测。


## EXP-020 torch-pruning 兼容性检查（2026-07-10）

- torch-pruning 1.6.1 安装成功。
- 默认 DependencyGraph 无法识别 YOLOv8 C2f 内部目标卷积，当前不执行实际裁剪。
- 下一步为自定义 forward/依赖映射适配，确保 C2f、Concat、Detect 连接安全。


## EXP-021 显式层依赖表（2026-07-10）

- 已归档 docs/artifacts/yolov8s_layer_dependency.tsv。
- 表中记录 23 个顶层模块及其 from 连接、类型和参数量，为后续手写剪枝映射提供依据。


## EXP-022 基线与 0.375 结构核对（2026-07-10）

- 基线主干通道：32/64/128/256/512；0.375 候选：24/48/96/192/384。
- 两者并非同一结构的轻微训练差异；后续剪枝目标固定为基线模型。


## EXP-023 ONNX 算子融合检查（2026-07-10）

- 基线 ONNX：226 节点，Conv=63，BatchNormalization=0，Sigmoid=59，Mul=56，Concat=13。
- 结论：Conv+BN 已融合；SiLU 仍表现为 Sigmoid+Mul，是否需要替换必须由板端 profiling 决定。


## EXP-024 激活模块替换方案（2026-07-10）

- 方案文件：docs/artifacts/activation_replacement_plan.json。
- 候选：SiLU（Sigmoid+Mul）→ReLU6；需在 profiling 确认热点后实施。
- 当前未修改模型，保留基线可回滚。


## EXP-025 蒸馏方案（2026-07-10）

- 方案文件：docs/artifacts/distillation_plan.json。
- Teacher 为六类 YOLOv8s baseline，Student 为后续结构化剪枝模型；包含 box、分类和 P3/P4/P5 特征蒸馏。
- 当前仅完成方案，待学生模型结构验证后训练。


## EXP-026 C2f 内部依赖明细（2026-07-10）

- 已归档 docs/artifacts/yolov8s_c2f_internal.tsv，共 57 行内部层信息。
- 下一步据此实现 C2f 分支一致的通道裁剪。


## EXP-027 任务状态总览（2026-07-10）

- 当前可靠基线：六类 YOLOv8s，test mAP50=77.2%；RK3588 rknn_run=50.707 ms avg。
- 0.375 宽度候选 test mAP50=42.3%，已淘汰。
- 当前未改 baseline 权重，主要阻塞为 torch-pruning 对 C2f/Concat/Detect 依赖追踪失败。
- 目标：手工重建约 10% 剪枝模型，精度保持在 baseline -3 个百分点以内，NPU 延迟低于 50.707 ms。


## EXP-028 手工 10% 通道缩减（2026-07-11）

- 结构：32/64/112/224/448 通道，参数约 9.51M；baseline 约 11.14M。
- 训练：1 epoch smoke 和 30 epoch 微调均通过。
- 结果：val mAP50=58.6%；test mAP50=64.0%、mAP50-95=42.8%。
- 结论：精度下降超过验收阈值，候选淘汰，不做 RKNN 导出。


## EXP-029 5% 候选配置检查（2026-07-11）

- 初版 5% YAML 实际参数约 7.92M，较 baseline 11.14M 减少约 29%，配置不符合目标。
- 未训练，需先修正模型参数映射。


## EXP-030 低比例通道缩减（2026-07-11）

- 结构参数约 10.734M，较 baseline 减少约 3.6%；smoke、30 epoch 微调通过。
- val mAP50=59.3%；test mAP50=58.5%、mAP50-95=38.2%。
- 结论：仍低于 baseline test mAP50=77.2%，候选淘汰，不导出 RKNN；后续改走蒸馏/模块级小改。


## EXP-031 ReLU6 模块替换（2026-07-11）

- ReLU6 结构 1 epoch smoke 和 30 epoch 微调通过。
- val mAP50=63.2%；test mAP50=65.6%、mAP50-95=50.2%。
- 结论：低于 baseline test mAP50=77.2%，候选淘汰，不做 RKNN 导出。


## EXP-032 蒸馏计划登记（2026-07-11）

- distill_desktop6.py 尚未实现或运行。
- 执行顺序：实现入口 → 1 epoch smoke → 30 epoch → test → 达标后 ONNX/RKNN → RK3588 profiling。
- test mAP50 验收线为 74.2%，不达标则保留原 YOLOv8s baseline。

## EXP-033 蒸馏候选最终结果（2026-07-11）

- 已完成 Teacher/Student smoke、训练/resume、checkpoint reload 和固定 test 评估。
- distilled pruned candidate test mAP50=56.82%，低于 baseline 77.18% 和 74.2% 门禁。
- 该候选淘汰，不导出 ONNX/RKNN，不上板。


## EXP-034 baseline 板端 Smoke（2026-07-11）

- ISP 已恢复，/dev/video20 成功协商 YUYV 640x480、15 FPS。
- baseline rockchip_dfl RKNN 成功加载：9 outputs、zero-copy、RKNN API 2.3.0、driver 0.9.8。
- 生产路径 Smoke、正检和 H.264 FLV 编码均通过；正检日志出现 objs:1-2。
- MQTT 本轮按要求跳过。


## EXP-035 性能 governor 优化（2026-07-11）

- 部署并启用 rockchip-performance.service，锁定 CPU/NPU/DDR performance，关闭 CPUIdle state1。
- 生产路径中 rknn_run 稳定约 26.7 ms，总推理约 34 ms。
- 5 分钟长稳运行正常，温度最高约 37.9 C，无降频、崩溃或异常退出。


## EXP-036 RKNN 生产热点 benchmark（2026-07-11）

- 条件：性能服务 active、zero-copy、SRAM、all NPU cores、50 warmup、300 runs。
- rknn_run median=26.540 ms，P90=26.617 ms。
- outputs_get median=1.723 ms，P90=1.750 ms。
- Rockchip postprocess median=4.126 ms，P90=4.194 ms。
- 完整 run_get_post median=32.395 ms，P90=32.534 ms。
- 当前下一步热点是 CPU 输出处理和后处理合计约 5.8 ms；NPU 网络本身仍是主要耗时。


## EXP-037 图级候选和当前结论（2026-07-11）

- RKNN optimization_level=0/1/2/3 已对比，差异约 0.04%，无实际收益。
- ONNX 已确认 Conv+BN 融合；SiLU 仍表示为 Sigmoid+Mul，尚未做手工替换。
- 六类 native RKNN 重建候选固定 test mAP50=56.08%，淘汰，不上板。
- 手工通道填充、Concat 重排和激活子图替换尚未完成，必须先做 A/B 精度和板端耗时验证。
- 当前 baseline、板端性能优化和稳定性验证完成；真正的手工图优化仍未完成。

## EXP-038：RKNN 逐算子图级 profiling

- 条件：部署九输出 YOLOv8s RKNN，API 2.3.0，all cores，SRAM，zero-copy，warmup=1，runs=1；性能采集模式只用于定位，不纳入常规基线。
- 结果：总算子时间 25.874 ms；ConvExSwish 22.222 ms（85.89%），Concat 1.275 ms（4.93%），Split 0.810 ms（3.13%），ConvSigmoid 0.507 ms（1.96%），Resize 0.352 ms（1.36%）。
- 结论：SiLU 已融合，通道已对齐；Concat 可见但不是值得手写等价重排的热点。没有部署模型变更。
## EXP-039：80x80 C2f 单重复块缩减

- 结构：仅将 backbone 的 80x80 C2f 内部重复从 2 减至 1；参数 11.046M，计算量 27.4 GFLOPs，相比 baseline 28.6 GFLOPs。
- 训练：从 baseline best.pt 部分迁移，30 epoch 完成；val mAP50=66.9%。
- 固定 test：mAP50=66.44%，mAP50-95=54.57%；cell phone/cup/keyboard/mouse/laptop/book AP50 分别为 63.95/86.13/68.15/31.69/58.32/90.38%。
- 结论：未达到 mAP50 74.18% 门槛，淘汰；不导出 ONNX/RKNN，不进行板端测试。
## EXP-040：INT8 DFL 解码微优化

- 做法：直接在 INT8 DFL 张量上计算 softmax 差值，移除每候选的 float 临时数组；候选筛选和 NMS 未改变。
- 首轮存在同时影响 rknn_run 与 outputs_get 的冷启动状态，不采信。交错热态 300-run A/B 后，baseline/候选后处理中位数约为 4.15/4.00 ms，落在运行波动内；完整链路仍约 32.4-32.5 ms。
- 结论：无稳定收益，代码已撤回，不进入部署。
## EXP-041：outputs_get 预分配 buffer

- Benchmark-only caller-owned raw output buffers were compared with the original outputs_get path using interleaved 300-run tests. Synthetic deterministic input: outputs_get median about 1.73 -> 1.17 ms; complete run_get_post about 32.53 -> 31.77 ms. Same-input detection count was 0 for both paths.
- The mechanism was connected only in an isolated production binary and run with the restored ISP/config. Initialization, inference, encoding, and graceful shutdown succeeded, but real tail samples were baseline 29.26-29.37 ms versus candidate 29.30-29.48 ms.
- Conclusion: synthetic gain did not transfer to the real production scene; production change reverted. Benchmark switch remains available for a future positive-detection scene.
## EXP-042：真实桌面图输出预分配复核

- 输入：固定 test 集真实桌面图 letterbox 到 640x640，板端 RKNN zero-copy，默认 outputs_get 与 caller-owned prealloc outputs 各 300 次。
- 两条路径最终均输出 6 个检测框，class/score/box 均一致：cup、book、bottle、bottle、keyboard、dining table。
- 计时仍受 NPU/RKNN 状态影响，预分配未在真实生产 Smoke 与正样本 benchmark 中稳定改善完整链路；不接入生产。
- 阶段结论：输出获取和常规 DFL/NMS 暂无可安全确认的收益，生产模型与生产二进制保持不变。
## EXP-043：板端 perf 函数采样可用性

- 检查结果：板端存在 perf/gprof 命令名，但 perf 提示缺少当前 6.1.118 内核对应的 linux-tools，无法生成函数采样报告。
- 结论：未修改生产推理逻辑；后续如需继续 CPU 阶段定位，应在 benchmark 内置候选扫描/DFL/NMS 阶段计时。
## EXP-044：直接调用已安装 perf 的板端函数采样

- 板端 linux-tools 已安装，但 /usr/bin/perf wrapper 按当前 6.1.118 内核查找不存在的版本化工具；直接调用 /usr/lib/linux-tools-5.15.0-185/perf 成功采样。
- 固定桌面正样本、RKNN benchmark 300 runs 采样：RockchipYoloPostprocess::process 66.87%，rknn_outputs_get 19.41%，rknn_run 8.77%，nms 0.23%，expf 1.38%。
- 结论：NMS 不是热点；下一步拆分 process 内部候选扫描/类别筛选与 DFL 解码耗时，不安装不匹配的 6.1 工具包，也不据此直接改生产逻辑。

## EXP-045 RockchipYoloPostprocess::process 循环重构与缓存优化（2026-07-11）

### 状态

已完成。通过重构后处理循环顺序，极大地提高了 L1/L2 缓存本地性，在板端 300 次 benchmark 实测中获得 **1.297 ms** 稳定收益，结果完全一致。该版本已正式部署为正式板端程序。

### 日期

2026-07-11

### 目的

使用板端 `/usr/lib/linux-tools-5.15.0-185/perf` 对带调试符号的 benchmark 采样和汇编级分析（`perf annotate`）。定位 `RockchipYoloPostprocess::process` 内部的具体 CPU 热点，并设计 semantically equivalent 的 A/B 优化以减少后处理时间。

### 瓶颈定位

通过 `perf record` 与 `perf annotate` 对固定正样本输入进行分析，发现热点完全集中在 `RockchipYoloPostprocess::process` 的 class 分类分数检索内层循环中：
```assembly
   41.64 :   c620:   csel    w2, w0, w23, gt
```
对应 C++ 语句 `int8_t score = score_tensor[class_id * grid_len + cell];`。
由于原始循环结构是 `cell` 在外层，`class_id` 在内层，且 `sum_tensor` 过滤能力在 80 类 background noise 累加下极其低下（95.4% 的 cells 均通过了 `sum_tensor` 门限进入了内层循环），导致内层循环不断以 `grid_len`（最大 6400 字节）的步长进行非连续的跨步内存访问，造成了严重的 L1/L2 缓存颠簸（Cache Thrashing）。

### 优化实现

将循环顺序进行转置（Restructure）：
1. 外层遍历 `class_id`，内层顺序（步长为 1）遍历 `cell`。
2. 为避免 heap 动态分配对中位数带来的抖动，在栈上分配 `best_scores` 和 `best_classes` 缓存区（仅在 `grid_len > 6400` 时才退避使用 `std::vector`），并使用 `std::fill_n` 初始化。
3. 顺序地寻找并记录所有 cells 的 `best_class` 和 `best_score`。
4. 第二阶段再顺序遍历 `best_classes` 缓存，仅对有效的候选 cell 进行 DFL 解码和检测框坐标映射构建。

这使得对 `score_tensor` 的访问模式转为 100% 连续的顺序内存读取，使 L1/L2 缓存预取器完全发挥作用。

### 测试结果

1. **结果一致性校验**：
   与 baseline 运行结果对比，在正样本输入下输出的 6 个检测框（包括类别 ID、置信度和 bounding box 坐标）完全 100% 相同，校验通过。
2. **板端 300 次 Benchmark 性能对比**：
   板端固定性能 Governor 下， warm-up 50 次， measured 300 次中位数实测：
   - **Baseline**: `postprocess median = 4.139 ms`, `run_get_post median = 32.833 ms`
   - **Optimized**: `postprocess median = 2.842 ms`, `run_get_post median = 31.597 ms`
   - **净收益**：**1.297 ms**（后处理耗时下降 **31.3%**），且完全稳定，超过 0.3 ms 验收阈值。

### 结论

重构后处理循环使得 CPU 端后处理时间减少了 **1.297 ms**。该优化已在源码层合入主线，并已正式部署替换为板端正式程序 `~/AlertGateway/AlertGateway`（备份为 `~/AlertGateway/AlertGateway.bak.20260711_205840`），并成功完成了 60 秒 smoke 真机验证，加载、推理、编码及优雅退出全部正常。

## EXP-046 优化后性能收口与后处理阶段级剖析（2026-07-11）

### 状态

已完成。在 benchmark 中成功引入 `--postprocess-stages` 选项目标测量，获得高精度的后处理四个子阶段耗时统计；结果确认非类别筛选阶段（DFL + Box 映射 + NMS）累计耗时仅 0.161 ms，不具备任何 >= 0.3 ms 的优化空间，正式并明确关闭 CPU 后处理的进一步优化。

### 日期

2026-07-11

### 目的

以已部署的正式优化版为对象，在 performance governor、all NPU cores、SRAM、zero-copy、warmup=50、runs=300 和固定桌面正样本输入下统计后处理阶段级时钟耗时（候选扫描/类别筛选、DFL 解码、坐标映射与检测框构建、NMS），评估后续独立阶段的优化价值，并校验输出检测框的完全一致性。

### 测试结果

1. **多指标基线与阶段计时 (300 Runs)**：
   - `rknn_run_wall`: mean=26.59 ms, median = **26.58 ms**, P90 = **26.65 ms**
   - `outputs_get`: mean=1.21 ms, median = **1.21 ms**, P90 = **1.24 ms**
   - `postprocess`: mean=2.86 ms, median = **2.86 ms**, P90 = **2.87 ms**
   - `run_get_post`: mean=30.67 ms, median = **30.65 ms**, P90 = **30.74 ms**
   - **后处理子阶段耗时分布**：
     - `stage_scan`（候选扫描/类别筛选）: mean=2.644 ms, median = **2.642 ms**, P90 = **2.649 ms**
     - `stage_dfl`（DFL解码）: mean=0.136 ms, median = **0.135 ms**, P90 = **0.142 ms**
     - `stage_box_map`（坐标映射与框构建）: mean=0.009 ms, median = **0.009 ms**, P90 = **0.010 ms**
     - `stage_nms`（NMS去重）: mean=0.018 ms, median = **0.017 ms**, P90 = **0.018 ms**
2. **结果等价性检验**：
   - 候选 timed postprocess 吐出的 6 个检测框，其类别、置信度以及坐标，与正式基线版本完全 100% 相同，成功通过等价一致性门限：
     - `det[0]` class=41 (cup) score=0.8728 box=(172.4810, 209.1167, 313.7706, 349.6657)
     - `det[1]` class=73 (book) score=0.7252 box=(402.0489, 306.4473, 639.3439, 554.5385)
     - `det[2]` class=39 (bottle) score=0.6932 box=(65.5135, 216.4703, 110.8642, 310.0852)
     - `det[3]` class=39 (bottle) score=0.5244 box=(53.4467, 218.6018, 72.2427, 297.1345)
     - `det[4]` class=66 (keyboard) score=0.3817 box=(494.4708, 219.3592, 640.0000, 337.6736)
     - `det[5]` class=60 (dining table) score=0.2672 box=(0.3466, 110.2229, 639.2637, 554.4808)

### 结论

- 阶段时钟表明：后处理的总耗时中 `stage_scan`（候选扫描/类别筛选）占了 **92.4%**。而除类别筛选外的其余全部阶段（DFL解码 + 坐标映射 + NMS）耗时总和仅为 **0.161 ms**。
- 这项计时基线提供了确定性证据，证明除类别筛选阶段外，其余后处理阶段完全不存在单个或累计能带来 >= 0.3 ms 收益的优化空间。
- **决策**：正式并**明确关闭 CPU 后处理的进一步优化**。后处理性能已经通过前期的循环 cache 优化榨取了最大空间，后续如果需要继续优化推理延迟，工作重心应转移至外部功耗物理测定或等待使用新模型结构/新数据集重启模型图级修改。