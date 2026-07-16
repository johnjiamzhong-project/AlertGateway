# 指定 4K 文件拉流与 V4L2 输出地址复用测试（2026-07-14）

## 测试条件

- 输入文件：`runs/input_videos/4k/VID_20260712_131410.mp4`
- 输入参数：3840×2160、H.264、`yuvj420p`、30 FPS
- 板端视频源：`pull_stream`，从 `rtmp://192.168.0.168/live/testsrc2` 拉流
- 输出地址：复用临时 V4L2 地址 `rtmp://192.168.0.168/live/v4l2_720p_result`
- 输出配置：MPP H.264、CBR 6000 kbps、3840×2160、30 FPS

## 结果

- `PullStreamThread` 成功连接并打开 3840×2160 `yuvj420p` 解码器。
- SRS API 确认共享输出地址发布活跃，视频为 H.264 Main、3840×2160。
- 启动稳定后，观测到输出约 30.34～32.51 FPS、约 6.7 Mbps；后续窗口有一段降至约
  26.30 FPS，说明 4K 30 FPS 在当前板端负载下仍存在波动，不能宣称全程锁定 30 FPS。
- 稳定窗口 `put_fail=0`、`out_drop=0`、`write_fail=0`、队列深度为 0；启动阶段曾发生
  一次 RTMP 写入重连和 12 个输出包丢弃，重连后恢复正常。

原始板端日志：
`runs/testsrc2/board_pull_vid_20260712_131410_shared_output_20260714.log`。
