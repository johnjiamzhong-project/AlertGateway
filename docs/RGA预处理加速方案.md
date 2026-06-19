# 优化计划：RGA 硬件加速 InferThread 预处理

> 状态：评估完成，待实施。

## 背景

`docs/NPU官方demo测速对比记录.md` 确认了 `rknn_run`(~40ms)已是 NPU 硬件极限，无法继续优化。但 `InferThread.cpp` 里的预处理(`yuyv_to_rgb` + `resize_rgb`，纯手写 CPU 标量循环)实测占单帧 ~13-14ms，且完整推理流程总耗时(~78-82ms)已经逼近摄像头实际出帧间隔(~68ms / 14.6fps)，是需要 `infer_every_n_frames` 跳帧的直接原因之一。

调研确认 RK3588S 的 RGA(2D 图形加速器)硬件原生支持 `RK_FORMAT_YUYV_422`，可以一次硬件调用完成"YUYV→RGB888 + 缩放到模型输入尺寸"，预计把这部分压到个位数 ms。

## 已确认的关键事实

- 板子 `librga2`/`librga-dev` 版本 **2.2.0-1**，`/usr/include/rga/` 头文件齐全(`im2d.h`/`rga.h`/`im2d_type.h` 等)，库文件 `/usr/lib/aarch64-linux-gnu/librga.so.2.1.0`，已在 ldconfig 缓存里，运行时无需额外部署或设 `LD_LIBRARY_PATH`。
- 用新版 **im2d API**(`improcess()`/`wrapbuffer_virtualaddr()`/`imStrError()`)，不是老的 `RgaApi.h` C 接口。参考实现见 `rknn_model_zoo/utils/image_utils.c` 的 `convert_image_rga()`，但那层封装不支持 YUYV，本次直接用底层 im2d API 自己 wrap YUYV 格式。
- `improcess` 普通堆内存(`std::vector<uint8_t>`)即可，不需要 DMA-BUF/ION。
- im2d API 无状态，不需要显式 init/deinit。
- 历史教训：paho-mqtt-cpp 和 librknnrt 都因交叉编译库版本与板子实际版本不一致踩过 ABI 坑(见 `BUGS.md`)。本次直接用板子上的真实 `librga.so.2.1.0` 做链接 stub，不从其他渠道下载不同版本。

## 改动范围

**新增：**
- `third_party/rga/include/*.h`(从板子 `/usr/include/rga/*.h` 整目录同步)
- `third_party/rga/lib/librga.so.2.1.0`(从板子同步真实库文件)

**修改 `CMakeLists.txt`**：参照现有 RKNN/MPP/Paho 的绝对路径风格，加 include 路径 + `RGA_LIB` + 链接。

**修改 `src/infer/InferThread.hpp`**：加 `#include "im2d.h"` / `#include "rga.h"`，不新增成员变量。

**修改 `src/infer/InferThread.cpp`**：
1. 保留现有 `yuyv_to_rgb()`/`resize_rgb()` 作为 RGA 失败时的 CPU fallback。
2. 新增 `rga_yuyv_to_rgb_resize()`：`wrapbuffer_virtualaddr` 包装 YUYV422 源 / RGB888 目标，`srect`/`drect` 用全图尺寸(整图拉伸，不做 letterbox，与现有行为一致)，调用 `improcess()`，返回值 `<= 0` 判失败。
3. `run()` 里用一次 `rga_yuyv_to_rgb_resize` 替换原来两次调用；失败则 fallback 回 CPU 路径，失败日志做节流。
4. 现有计时打印格式(`[Infer] cpu:.. cpy:.. npu:.. cnv:.. total:..`)保持不变，`cpu:` 字段语义变为"RGA(或 fallback CPU)预处理耗时"。

**不需要改动**：`Frame.hpp`、`CaptureThread.cpp`、`cmake/aarch64-toolchain.cmake`、`config/config.json`(不做运行时开关)。

## 验证步骤

1. 交叉编译，确认无 undefined symbol。
2. 部署到板子，确认启动无 `error while loading shared libraries`。
3. 观察 `[Infer]` 日志：`cpu:` 字段应从 13-14ms 降到个位数 ms，且不出现 `RGA improcess failed` / fallback 日志。
4. 对比改造前后同场景检测结果(MQTT 上报的 objects/置信度)，确认色彩转换没有明显影响精度。
5. (可选)先写独立 smoke test 程序单独验证 `improcess`，再合入 InferThread。

## 相关文件

- `npu_optimization_results.md`(memory)、`docs/NPU官方demo测速对比记录.md` —— NPU 侧已确认无优化空间的前置结论
- 完整实施计划存档：`/home/rambos/.claude/plans/rga-dreamy-valley.md`
