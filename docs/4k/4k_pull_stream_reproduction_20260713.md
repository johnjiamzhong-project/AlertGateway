# 4K 拉流处理卡顿与画质复现 (2026-07-13)

## 目的

复现 4K 视频拉流、处理并再次推流时报告的卡顿和严重画质下降问题。本次仅记录基线，未修改处理实现。

## 链路与配置

`VID_20260712_131410.mp4` -> WSL FFmpeg publisher -> SRS `live/testsrc2` -> RK3588 `AlertGateway` -> SRS `live/testsrc2_result`.

- Input: 3840x2160 H.264 High `yuvj420p`, about 30.03 FPS, about 42.8 Mbps.
- Board config: `runs/testsrc2/config_testsrc2_4k.json`, source 3840x2160@15, `rockchip_dfl`, Thumbnail/ROI/Tiling disabled.
- Encoder: MPP H.264 3840x2160@15, CBR target 3000 kbps, GOP 8, detection labels enabled.
- Board: Firefly RK3588S `192.168.0.200`; SRS: `192.168.0.168`.

## 结果

第二次受控运行持续约 28 秒，拉流、解码、推理、编码、MQTT 以及超时后的正常退出均成功完成。

- Decode opened 3840x2160 `yuvj420p` successfully.
- Inference total was about 36.5-49.1 ms/frame, commonly 42-45 ms; NPU was about 25.8-29.2 ms, CPU preprocessing about 9-14 ms, and color conversion about 1.5-6.1 ms.
- An 18-second capture of the output measured 3840x2160 H.264, nominal 15 FPS, average about 15.17 FPS, about 3.11 Mbps, and 17.999 seconds duration.
- 在这段短时抓取中，未发现输出帧率持续低于 15 FPS 的证据。但 4K 输出约 3 Mbps、而输入约 42.8 Mbps，已确认是造成严重画质下降的重要因素。
- The FLV `Failed to update header` messages occurred only during intentional shutdown/capture finalization, not during steady-state processing.

## 当前判断

基线结果表明，问题可能由 4K 全帧处理负载较高和输出码率过低共同造成。日志尚不能将播放端卡顿归因于单个线程：推理约为 40-45 ms/帧，软件解码/色彩转换、队列调度、MPP 编码、RTMP 时间戳和播放器缓冲仍需分别测量。

在修改模型或检测逻辑前，应保留此基线，并对比 3/6/8/12 Mbps 输出，同时记录输出时间戳、队列深度、编码耗时和接收端帧间隔。

原始产物:

- `runs/testsrc2/board_4k_repro_20260713.log`
- `runs/testsrc2/4k_repro_metrics_20260713.txt`
