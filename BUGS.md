# AlertGateway Bug 记录

---

## BUG-001 paho-mqtt-cpp ABI 不匹配

**现象**
```
./AlertGateway: undefined symbol: _ZN4mqtt15connect_options17set_clean_sessionEb
```
随后又出现：
```
undefined symbol: _ZN4mqtt12async_client7publishENS_10buffer_refIcEES2_ibRKNS_10propertiesE
undefined symbol: _ZN4mqtt12async_client6createEv
```

**原因**

交叉编译时链接的是本地新版 paho-mqtt-cpp（从源码 build），板子上安装的是 `libpaho-mqttpp3 1.2.0`。
新版头文件引入了旧版没有的符号：
- `set_clean_session(bool)` — 新版非 inline，旧版 inline
- `publish(buffer_ref, buffer_ref, int, bool, properties const&)` — 新版多了 `properties` 参数
- `async_client::create()` — 新版构造函数内部调用，旧版不存在

**处理方法**

1. 从本地 paho-mqtt-cpp 仓库 checkout 到与板子一致的 v1.2.0 版头文件：
   ```bash
   cd /home/rambos/paho.mqtt.cpp
   git archive v1.2.0 src/mqtt/ | tar -x --strip-components=1 \
       -C /home/rambos/arm_test/AlertGateway/third_party/paho/include/
   ```

2. 把板子上的 `.so` 文件 scp 回来作为链接存根（保证链接时符号一致）：
   ```bash
   scp firefly@192.168.1.200:/usr/lib/aarch64-linux-gnu/libpaho-mqttpp3.so.1 third_party/paho/
   scp firefly@192.168.1.200:/usr/lib/aarch64-linux-gnu/libpaho-mqtt3as.so.1  third_party/paho/
   ```

3. 更新 `CMakeLists.txt`：
   ```cmake
   set(PAHO_C_LIB   ${CMAKE_SOURCE_DIR}/third_party/paho/libpaho-mqtt3as.so.1)
   set(PAHO_CPP_LIB ${CMAKE_SOURCE_DIR}/third_party/paho/libpaho-mqttpp3.so.1)
   ```

**经验**

交叉编译时，头文件版本必须与板子上运行的 `.so` 版本完全一致。
最安全的做法：直接从板子 scp `.so`，再从对应 git tag 提取头文件。

---

## BUG-002 FFmpeg 头文件版本与运行库不匹配导致段错误

**现象**
```
Segmentation fault (core dumped)
```
gdb 定位到 `avformat_new_stream()` 内部崩溃。

**原因**

sysroot 里同时存在两套 FFmpeg：
- `/home/rambos/sysroot/usr/include/aarch64-linux-gnu` — 系统 FFmpeg **4.4.2** 头文件
- `/home/rambos/sysroot/opt/ffmpeg-rockchip/lib` — ffmpeg-rockchip **6.1** 运行库

`CMakeLists.txt` 只加了系统头文件路径，链接的却是 rockchip 6.1 的 `.so`。
FFmpeg 6.1 的 `AVFormatContext`、`AVStream` 等结构体布局与 4.4.2 不同，
导致字段偏移错误，调用 `avformat_new_stream` 时直接访问非法内存。

**处理方法**

在 `CMakeLists.txt` 中把 ffmpeg-rockchip 头文件路径加到系统头文件路径**之前**：

```cmake
# ffmpeg-rockchip 6.1 头文件必须先于系统 FFmpeg 4.4.2 头文件
include_directories(${SYSROOT}/opt/ffmpeg-rockchip/include)
include_directories(${SYSROOT}/usr/include/aarch64-linux-gnu)
```

**经验**

链接哪个 `.so` 就必须 include 对应版本的头文件。
有多套同名库时，`include_directories` 的顺序决定哪套头文件生效，顺序错误编译不报错但运行崩溃。

---

## BUG-003 draw_hline 越界写入导致堆损坏

**现象**
```
double free or corruption (!prev)
Aborted (core dumped)
```
gdb 定位到 `DetectionReporter::run()` 内的 `free()` 调用崩溃。

**原因**

`draw_hline` 函数只检查了 `y < 0`，没有检查 `y >= h`：

```cpp
// 修复前
static void draw_hline(uint8_t* rgb, int w, int x0, int x1, int y, ...) {
    if (y < 0) return;   // ← 缺少 y >= h 的检查
    ...
}
```

YOLOv8 后处理输出的检测框坐标经过坐标变换后，`y2` 可能恰好等于 `frame.height`（480）。
此时 `draw_rect` 调用 `draw_hline(..., y2 - 0)` 即 `y = 480`，
写入位置为 `rgb + (480 * 640 + x) * 3`，超出 `rgb_data` 缓冲区末尾，
破坏了相邻堆块的元数据，后续 `free()` 触发 abort。

**处理方法**

为 `draw_hline` 添加 `h` 参数并补充上界检查，同步更新所有调用处：

```cpp
// 修复后
static void draw_hline(uint8_t* rgb, int w, int h, int x0, int x1, int y, ...) {
    if (y < 0 || y >= h) return;   // ← 同时检查上下界
    ...
}
```

`draw_rect` 调用也同步改为传入 `h`：
```cpp
draw_hline(rgb, w, h, x1, x2, y1 + t, r, g, b);
draw_hline(rgb, w, h, x1, x2, y2 - t, r, g, b);
```

**经验**

对 RGB/YUV 缓冲区做像素操作时，x/y 坐标必须同时检查上下界。
目标检测的坐标经过缩放/裁剪后容易出现边界值（刚好等于宽/高），需要特别防范。

---

## BUG-004 SSH 间歇性超时（ping 正常）

**现象**

SSH 连接持续报错，无论 AlertGateway 是否运行：
```
ssh: connect to host 192.168.1.200 port 22: Connection timed out
```
但 `ping 192.168.1.200` 始终正常，`nc -zv 192.168.1.200 22` 端口也是通的。

**初始误判**

最初怀疑是 AlertGateway 推流（2Mbps RTMP）占满带宽导致 SSH 握手超时。
但事后验证：**AlertGateway 未运行时 SSH 同样超时**，排除推流是主因。

**真实原因**

检查板子网络接口状态：
```
eth0:  NO-CARRIER, DOWN   ← 网线未插
wlan0: DORMANT, DOWN      ← 内置 WiFi 未连接
p2p0:  UP                 ← 实际在用此接口
```

板子通过 **USB WiFi 网卡（Realtek 0bda:c820 802.11ac）的 P2P 虚拟接口（`p2p0`）** 联网，
网卡经 USB Hub 间接接入，并非有线 `eth0` 或内置 WiFi。

导致 SSH 不稳定的原因：
- USB WiFi 本身延迟高、丢包率高于有线网络
- USB Hub 增加了一层传输瓶颈
- P2P 模式（WiFi Direct）比普通 STA 模式更不稳定
- RTMP 推流、SSH、NPU 推理三者共用同一 USB WiFi 信道，互相竞争

ping 始终正常是因为 ICMP 包极小，内核优先处理，不走 TCP 拥塞控制队列。

**处理方法**

1. **临时绕过**：SSH 超时时多重试几次，一般能在 WiFi 空档期建立连接：
   ```bash
   for i in 1 2 3; do
     ssh -o ConnectTimeout=15 firefly@192.168.1.200 "echo ok" && break
     sleep 3
   done
   ```

2. **根本解决：插网线**

   板子有千兆有线口 `eth0`，插上网线后 SSH 与推流完全走不同通道，
   两者互不干扰，SSH 稳定性立即恢复。

3. **推流期间需要维护时**：先停服务再操作，减少带宽竞争：
   ```bash
   ~/app/AlertGateway/start.sh stop
   # 维护...
   ~/app/AlertGateway/start.sh start
   ```

**经验**

- `ping 通但 SSH 不通` 不一定是服务挂了，优先排查网络通道质量（WiFi vs 有线）
- 嵌入式设备尽量用有线网络；USB WiFi 经 Hub 的链路质量极不可靠
- 管理通道（SSH）与数据通道（RTMP）应走不同物理链路，生产环境用双网卡或有线+无线分离

---

## BUG-005 Pipeline 串行耦合导致帧率被 NPU 推理速度拖死

**现象**

RTMP 推流画面严重卡顿，实际帧率约 5fps，远低于摄像头能力（14.6fps）。

**原因**

原始 pipeline 为串行结构：

```
CaptureThread → [queue] → InferThread → [queue] → EncodeThread → StreamThread
```

- `InferThread` 每帧做 YUYV→RGB 全图转换（640×480×3 字节）+ RKNN NPU 推理，单帧耗时约 200ms（~5fps）
- `EncodeThread` 阻塞等待 `InferThread` 的输出，整条 pipeline 被 NPU 速度拖死
- 摄像头 14.6fps 的采集能力完全浪费

附加问题：`EncodeThread` 还在做 YUYV→RGB→NV12 的两次色彩转换，徒增 CPU 开销。

**处理方法**

**1. Pipeline 解耦为并行双路**

`CaptureThread` 同时向两个独立队列推帧：

```
CaptureThread ──→ [enc_queue=1]   → EncodeThread → StreamThread
              └──→ [infer_queue=2] → InferThread  → SharedDetections
```

- `enc_queue`：容量 1，阻塞推送（不丢帧）
- `infer_queue`：容量 2，非阻塞推送（InferThread 忙时直接丢帧）
- `InferThread` 每次取帧时排空队列，始终推理最新帧
- `EncodeThread` 以摄像头速度独立运行，从 `SharedDetections` 取最新检测结果画框

**2. 新增 `SharedDetections` 共享结构**

```cpp
struct SharedDetections {
    mutable std::mutex mutex;
    std::vector<Detection> detections;
    void set(std::vector<Detection> dets) { ... }
    std::vector<Detection> get() const { ... }
};
```

InferThread 写入，EncodeThread 无阻塞读取快照，两者完全解耦。

**3. EncodeThread 改为直接 YUYV→NV12**

消除 RGB 中间缓冲，YUYV 每像素对直接提取 Y/U/V 写入 NV12 两个 plane：

```cpp
void yuyv_to_nv12(const uint8_t* yuyv, uint8_t* nv12, int w, int h) {
    uint8_t* y_plane  = nv12;
    uint8_t* uv_plane = nv12 + w * h;
    for (int row = 0; row < h; row++) {
        // Y plane：每隔一字节取 Y
        // UV plane：仅偶数行取 U/V（2× 垂直降采样）
    }
}
```

NV12 画框颜色（绿色，BT.601）：Y=145, U=54, V=34

**效果**

解耦后 `EncodeThread` 帧率恢复到摄像头极限 14.6fps，NPU 推理速度不再影响编码推流。

---

## BUG-006 RTMP 延迟过高（15s）

**现象**

解决 BUG-005 后，推流画面流畅，但 RTMP 延迟达到 15 秒。

**原因排查**

| fps 配置 | 行为 | 现象 |
|---------|------|------|
| fps=30（错误值） | PTS 推进速度 2× 实际帧率 → SRS 缓冲被快速"消耗" | 延迟约 1s，但画面卡顿丢帧 |
| fps=15（正确值） | PTS 节奏正常 → SRS 和播放器填满默认大缓冲区 | 流畅但延迟 15s |

根本原因：fps=15 时 PTS 正常，但 SRS 默认 `gop_cache=on`，`queue_length` 较大，播放器（PotPlayer）也有 2-3s 默认缓冲，叠加后延迟 15s。

**处理方法**

**1. SRS 服务端配置低延迟**（在 Windows PC 的 SRS `rtmp.conf` 中）：
```
min_latency  on;
gop_cache    off;
queue_length 0.1;
mw_msgs      0;
```

**2. 编码端减少缓冲**：
- GOP：由默认值改为 8（关键帧间隔 ~550ms），减少 gop_cache 贡献
- H.264 profile = Main (77)，level = 3.1
- `enc_queue` 容量：1（约 68ms 缓冲）
- `stream_queue` 容量：2（约 137ms 缓冲）

**注意：GOP=2 的副作用**

初期将 GOP 设为 2（每 2 帧一个关键帧，~140ms），导致 I 帧频率过高：
- I 帧体积约为 P 帧的 5-10 倍，产生瞬时码率峰值
- 低延迟播放器（如 RambosPlayer，缓冲仅 0.5s）无法平滑峰值，表现为帧率下降
- 最终调整为 GOP=8（~550ms），码率更均匀，RambosPlayer 帧率恢复正常

**最终效果**

| 播放器 | 延迟 | 帧率 |
|--------|------|------|
| PotPlayer | 3-4s（播放器自身缓冲，无法绕过） | 14.6fps 正常 |
| RambosPlayer | ~0.5s | 14.6fps 正常（GOP=8 后） |

**经验**

- RTMP 延迟 = 服务端 gop_cache + 服务端队列 + 网络传输 + 播放器缓冲，需逐层排查
- GOP 过小虽然降低首帧延迟，但导致码率峰值，对低延迟播放器有害
- fps 配置必须与摄像头实际帧率一致，否则 PTS 错乱会引发各种隐性问题

**补充：为什么设置 fps=30 摄像头实际只输出 14.6fps**

V4L2 的 fps 设置是协商过程，不是强制命令：

```c
// VIDIOC_S_PARM 后驱动将实际协商值写回结构体
parm.parm.capture.timeperframe = {1, 30};  // 请求 30fps
ioctl(fd, VIDIOC_S_PARM, &parm);
// 驱动实际返回摄像头支持的最近档位
```

~~本项目摄像头在 640×480 YUYV 格式下只支持 15fps 这一档~~ —— **此前的结论不准确，已纠正**：
用 `v4l2-ctl -d /dev/video20 --list-formats-ext` 实际查驱动枚举出的能力表，640×480 YUYV
**声明支持 30/25/20/15/10/5fps 共 6 档**（MJPG 格式同分辨率下同样声明支持这 6 档），
并不是只有 15fps 这一档。板子用的是 USB 3.0 接口，带宽（YUYV 30fps 仅需 17.6MB/s）也远未跑满。

也就是说**"驱动声明支持的档位"和"实际能稳定吐出来的帧率"是两件事**：协商阶段驱动接受了
30fps 请求（`VIDIOC_S_PARM` 不报错），但实测（见下方验证方法）吞吐稳定在 14.6fps，跟带宽、
跟 USB 接口版本都对不上。真正瓶颈待进一步排查（可能是 sensor 在该模式下的真实曝光/读出耗时，
或 ISP/驱动内部处理耗时），暂不下结论，后续再查。

实际帧率 14.6fps（而非整 15fps）这个"非整数"的尾差，是 USB 调度抖动 + V4L2 buffer 管理开销
导致的微小偏差；但 14.6 与 30 之间这接近 2 倍的差距，不能只用这个尾差解释。

验证实际帧率的方法：
```bash
# 在板子上查摄像头支持的格式和帧率
v4l2-ctl -d /dev/video20 --list-formats-ext

# 通过 SRS API 测量推流实际帧率（取两次 frames 差值除以时间）
F1=$(curl -s http://<srs-ip>:1985/api/v1/streams/ | python3 -c "import sys,json; print(json.load(sys.stdin)['streams'][0]['frames'])")
sleep 5
F2=$(curl -s http://<srs-ip>:1985/api/v1/streams/ | python3 -c "import sys,json; print(json.load(sys.stdin)['streams'][0]['frames'])")
echo "fps: $(echo "scale=1; ($F2-$F1)/5" | bc)"
# 实测：5秒内 73帧 → 14.6fps
```

config.json 的 `"fps": 30` 目前仍为错误值，待后续修正为 15。

---

## BUG-007 NPU 推理速度仅 6-7 FPS，比行业基准慢 4-5 倍

**现象**

AlertGateway 实测推理总耗时约 150ms/帧（~6.7 FPS），行业基准 YOLOv8s 在 RK3588S 上应达到 27-32 FPS。

加计时代码后各阶段耗时：
```
cpu（YUYV→RGB + resize）:  8ms
rknn_inputs_set:           37ms
rknn_run（纯 NPU 计算）:   75ms
rknn_outputs_get:          37ms
total:                    150ms
```

**排查过程**

**Step 1：加计时，定位各阶段耗时**

在 `InferThread::run()` 中对四个阶段分别打点，确认 `rknn_run` 75ms 是最大单项，
数据搬运（inputs_set + outputs_get）合计 74ms 是第二大项。

**Step 2：rknn_run 75ms 异常——怀疑量化类型**

YOLOv8s INT8 在 RK3588S 上的参考值约 30-40ms，当前 75ms 整整高了一倍。
第一嫌疑是模型没有做 INT8 量化。用 `strings` 直接检查 rknn 文件内容：

```bash
strings ~/AlertGateway/model/yolov8s.rknn | grep -E 'dtype|quant|int8|float'
```

输出显示 `"dtype": "float16"`，量化参数段为空。
**结论：模型是 FP16，从未做过 INT8 量化，NPU 以 16 位浮点运算，天然比 INT8 慢一倍。**

**Step 3：排除多核 NPU**

测试 `rknn_set_core_mask(RKNN_NPU_CORE_0_1_2)` 启用三核并行，`rknn_run` 时间无变化。
YOLOv8s 计算图的算子依赖关系限制了多核并行收益，该方向放弃。

**处理方法**

**1. 采集 INT8 量化校准数据集（150 张）**

INT8 量化需要真实场景图来统计各层激活值分布，写采集脚本 `tools/collect_calibration.py`：
- 从 `/dev/video20` 捕获画面，带 OpenCV 预览窗口
- 空格键手动采集，D 键撤销，Q 退出
- 图片保存为 640×640 RGB PNG（与推理预处理尺寸一致）
- 通过 Moonlight+Sunshine 远程桌面在板子 Xfce 桌面上操作预览

**2. 重新量化转换（INT8）**

在 WSL x86_64 上用 rknn-toolkit2 对原始 ONNX 重新量化，写转换脚本 `tools/convert_int8.py`：

```python
rknn.config(
    quantized_dtype="asymmetric_quantized-8",
    quantized_algorithm="normal",
    ...
)
rknn.build(do_quantization=True, dataset="~/calibration/dataset.txt")
```

转换过程踩到两个坑：
- **onnx 版本冲突**：系统装的 onnx 1.22.0 移除了 `onnx.mapping`，rknn-toolkit2 依赖此接口。
  解决：降级到 `onnx==1.13.1`。
- **dataset 参数类型错误**：`rknn.build(dataset=...)` 只接受文件路径字符串，不接受 numpy 数组列表。
  解决：改为将图片路径写入 `dataset.txt` 后传文件路径。

**3. 替换板子上的模型文件**

```bash
scp ~/yolov8s_int8.rknn firefly@192.168.0.200:~/AlertGateway/model/yolov8s.rknn
```

代码无需改动：`outputs[i].want_float = 1` 让 runtime 自动将 INT8 输出反量化为 float，
后处理（`YoloPostprocess`）照常工作。

**效果**

| 阶段 | FP16（优化前）| INT8（优化后）| 说明 |
|------|-------------|-------------|------|
| rknn_run | 75ms | **38ms** | INT8 运算量减半 |
| inputs_set | 37ms | **0.4ms** | INT8 数据体积小（约 1/2），搬运快 |
| outputs_get | 37ms | **1-3ms** | 同上 |
| **total** | **~150ms** | **~48ms** | **3x 提速** |

推理能力从 6.7 FPS 提升至约 **20 FPS**，已超过摄像头实际帧率（14.6fps）上限。

**经验**

- 拿到 rknn 模型后，第一步用 `strings` 检查 `dtype` 字段确认量化类型，FP16 性能约为 INT8 的一半
- INT8 量化校准图要有代表性（真实场景、多角度、多光线），图片不足或质量差会导致精度下降
- rknn-toolkit2 只能在 x86_64 Linux 上运行（WSL 可用），不能在板子本身上做转换
- `rknn.build(dataset=...)` 传的是 **txt 文件路径**，不是 numpy 数组

---

## BUG-008 SRS 重启后 AlertGateway 不自动重连，编码帧率跌至 5fps

**现象**

SRS 服务器重启（或网络抖动导致 RTMP 连接断开）后，AlertGateway 的编码帧率从 14.6fps 跌至约 5fps，
RambosPlayer 收不到任何数据。只有手动重启 AlertGateway 才能恢复推流。

**原因**

`StreamThread` 使用 librtmp 向 SRS 推流，连接建立后没有断线检测和重连逻辑。
RTMP 连接断开时，`RTMP_SendPacket` 返回失败，StreamThread 进入错误状态但不退出，
阻塞在 `stream_queue.pop()` 或发送调用上。

`enc_queue` 容量=1，StreamThread 不消费时 EncodeThread 很快被背压阻塞，
CaptureThread 的 `enc_queue.push(100ms timeout)` 频繁超时，帧率跌到 ~5fps。

**复现步骤**

1. AlertGateway 正常推流（14.6fps）
2. 重启 SRS 服务器
3. 约 2-3 秒后 AlertGateway 日志出现 `Encode: 5 fps`，RambosPlayer 断流

**临时处理**

SRS 重启后手动重启 AlertGateway：
```bash
ssh firefly@192.168.0.200 "pkill AlertGateway; sleep 2; cd ~/AlertGateway && nohup ./AlertGateway config/config.json > /tmp/ag.log 2>&1 &"
```

**根本原因（三层）**

修复过程中发现三个叠加的 bug：

**Bug 1：断线检测失效（主因）**

原代码：
```cpp
int ret = av_write_frame(fmt_ctx_, pkt);
if (ret >= 0) avio_flush(fmt_ctx_->pb);  // flush 返回值未检查
return ret >= 0;
```
`av_write_frame` 把数据写入 TCP 发送缓冲区返回成功，但 `avio_flush` 真正触发 TCP 发送时才拿到 EPIPE/ECONNRESET。`avio_flush` 的错误被静默忽略，`write_packet` 一直返回 true，断线永远检测不到。

修复：
```cpp
int ret = av_write_frame(fmt_ctx_, pkt);
if (ret < 0) return false;
avio_flush(fmt_ctx_->pb);
return fmt_ctx_->pb->error >= 0;  // 检查 flush 后的错误字段
```

**Bug 2：重连后无法恢复推流（次因）**

MPP 编码器只在**启动时第一个关键帧**里带 SPS/PPS（type 7/8）。原 `close_rtmp()` 会清空 `sps_/pps_`，重连后等不到新的 SPS/PPS，`write_extradata` 永远不被调用，`header_written_` 始终为 false，`write_packet` 每次提前 `return true` 什么也不发。SRS API 表现为 `active: false`，players 无法拉流。

修复：重连时保留 `sps_/pps_`，连上后立刻用缓存值写 header：
```cpp
void StreamThread::reconnect_loop() {
    close_rtmp();  // 不再清空 sps_/pps_
    while (running_) {
        if (open_rtmp()) {
            if (!sps_.empty() && !pps_.empty())
                write_extradata(sps_.data(), sps_.size(), pps_.data(), pps_.size());
            return;
        }
        // 重试期间持续排空 in_queue_ 防止背压
    }
}
```

**Bug 3：重连失败后线程退出导致背压**

原代码重连一次失败即 `break` 退出线程，没人消费队列，背压传播至 EncodeThread → CaptureThread，帧率跌至 5fps。

修复：改为无限重试，每 3 秒一次，重试期间持续 `pop` 排空队列。

**Bug 4：SRS gop_cache 实际未关闭导致播放器周期性卡顿 ~550ms**

同一排查过程中发现 RambosPlayer 出现周期性冻帧约 550ms，日志特征：

```
VideoRenderer: no frame for 510 ms (queue empty)
VideoRenderer: render frame pts= 5.333 (after 578 ms)
VideoRenderer: no frame for 514 ms (queue empty)
VideoRenderer: render frame pts= 5.567 (after 547 ms)
```

相邻帧 PTS 差 234ms，但真实等待 547ms；每次重连后 pts 精确重置到同一值（5.177）。
这是 SRS `gop_cache` 实际生效的典型特征：GOP=8、14.6fps → GOP 周期 = 547ms，
SRS 攒满 8 帧后批量发送，播放器收到爆发包后等 547ms 没有新数据，形成冻帧感。

**排查弯路一：改了错误的配置文件**

启动脚本 `srs-live.bat` 的内容是 `objs\srs.exe -c conf\live.conf`，但实际进程命令行
（通过 `Get-WmiObject Win32_Process` 确认）显示加载的是 `conf\console.conf`。
先后改了 `srs.conf`、`live.conf`，两次修改均无效，重启也无效。

确认实际配置文件的方法：
```powershell
Get-WmiObject Win32_Process | Where-Object { $_.Name -like '*srs*' } | Select-Object ProcessId, CommandLine
# 输出：objs\srs.exe  -c conf\console.conf
```

**排查弯路二：HLS enabled 导致 gop_cache off 无效**

找到正确文件 `console.conf` 后，加入 `gop_cache off` 并重启，卡顿仍然存在。
原因：`console.conf` 中有 `hls { enabled on; }`。HLS 切片必须从 I 帧开始，
SRS 内部会为 HLS 维护 GOP 缓冲，这个缓冲路径与 RTMP play 路径耦合，
导致即使 play 块中写了 `gop_cache off`，RTMP 订阅者收到的仍是 GOP 批量数据。

**最终修复**

去掉 `hls`、`rtc`、`http_remux` 块，使用最简 realtime 配置：

```
vhost __defaultVhost__ {
    tcp_nodelay     on;
    min_latency     on;

    play {
        gop_cache       off;
        queue_length    10;
        mw_latency      100;
    }

    publish {
        mr              off;
    }
}
```

重启 SRS 后卡顿消失，帧以 ~68ms 间隔连续投递。

**经验**

- 先用进程命令行确认 SRS 实际加载的配置文件，不要猜
- HLS `enabled on` 会让 SRS 内部维护 GOP 缓冲，与 RTMP 的 `gop_cache off` 冲突；
  纯 RTMP 低延迟场景应禁用 HLS
- 参考 `conf/realtime.conf` 作为低延迟 RTMP 的标准配置模板

**处理结果**

修复后 SRS 重启全流程日志：
```
StreamThread: write failed, reconnecting...    ← avio_flush 错误检测到断联
StreamThread: reconnect failed, retry in 3s   ← SRS 未就绪，重试
StreamThread: reconnect failed, retry in 3s
StreamThread: reconnected to rtmp://...       ← 连上，立刻用缓存 SPS/PPS 写 header
Encode: 14.6 fps                              ← 数据正常推送，SRS active: true
```

全程 Encode 帧率不跌，RambosPlayer / PotPlayer 重连后可正常拉流。

**涉及文件**

`src/stream/StreamThread.cpp` — `write_packet`、`close_rtmp`、`reconnect_loop`、`run`

**经验**

- `avio_flush` 在此版本 FFmpeg 返回 void，错误要通过 `fmt_ctx_->pb->error` 读取
- MPP 编码器 SPS/PPS 仅首帧携带，重连逻辑必须缓存并复用，不能依赖等待新关键帧
- 断线重连期间必须持续消费下游队列，否则背压会让整条 pipeline 帧率崩溃（背压机制详见 BUG-005）
- 任何长连接服务（RTMP、MQTT、WebSocket）都必须有断线重连逻辑

---

## BUG-009 输出视频检测框始终为空（INT8 量化精度丢失）

**现象**

接入 RGA 预处理加速并修复一处反量化错误后，AlertGateway 运行正常、无报错，但输出视频里始终一个检测框都没有，调低 `conf_threshold` 也无效。

**原因（两层）**

1. `yolov8s.onnx` 导出时 box 坐标（4通道，像素范围 0-640）和分类概率（80通道，范围 0-1）被合并成一个 84 通道输出张量，量化时共享一组 scale/zp。这组 scale（约 2.6）是按 box 坐标的数值范围校准的，分类概率的整个 0-1 区间在这个 scale 下不到 1 个 INT8 量化档位，反量化回来全部精确变成 0，分类信息在量化这一步就已经丢失，运行时代码无法补救。
2. 排查中还发现一个独立的小 bug：YOLOv8 的 ONNX 图里分类分支自带 `Sigmoid` 节点（导出时已内置），但 C++ 后处理又手动做了一次 sigmoid，属于重复计算（即使第一层问题修复，这个也需要去掉）。

**处理方法**

用 `rknn.load_onnx(model=..., outputs=['/model.22/Mul_2_output_0', '/model.22/Sigmoid_output_0'])` 在 ONNX 图最后一次 concat **之前**把 box 和分类分支拆成两个独立输出，分别量化校准（`tools/convert_int8.py`）。C++ 侧改用 `rknn_outputs_get(want_float=1)` 取两个输出的浮点值（不再手动反量化，也不再重复 sigmoid），`YoloPostprocess::process()` 改为同时接收 box/cls 两个缓冲区。

**涉及文件**

`tools/convert_int8.py`、`src/infer/InferThread.cpp`、`src/infer/YoloPostprocess.cpp/hpp`

**经验**

- YOLO 系列模型导出 INT8 时，box 回归和分类两个分支数值量级差异巨大，必须拆成独立输出张量分别量化，合并成一个张量共享 scale 会直接摧毁其中一个分支的精度
- 怀疑反量化/精度问题时，先打印"全图最高分"和原始浮点值，不要只盯着调阈值——阈值调多低都救不回精度已经丢失的数据
- 详细排查过程见 `docs/检测框消失问题排查记录.md`
