# 4K 18 Mbps 推理推流验证（2026-07-14）

## 测试条件

- 输入文件：`runs/input_videos/4k/VID_20260712_131410.mp4`
- 输入拉流：`rtmp://192.168.0.168/live/testsrc2`
- 源配置：3840×2160、30 FPS、`pull_stream`
- 输出地址：`rtmp://192.168.0.168/live/v4l2_720p_result`
- 输出配置：`bitrate_kbps=18000`
- 板端日志：`/tmp/ag_pull_18m_shared_20260714.log`

## 入口配置切换与循环查看

- [config/config.json](../../config/config.json) 的 `active_config` 已切换为
  `config_4k_18mbps.json`，并部署到 201 板后使用统一入口启动：
  `./AlertGateway config/config.json`。
- WSL/主机循环发布输入视频的命令为：
  `ffmpeg -re -stream_loop -1 -i runs/input_videos/4k/VID_20260712_131410.mp4 -map 0:v:0 -c copy -f flv rtmp://192.168.0.168/live/testsrc2`。
- 201 板从 `live/testsrc2` 拉流，经过 RKNN 推理和 MPP H.264 编码后，输出到
  `rtmp://192.168.0.168/live/v4l2_720p_result`；播放器直接查看该地址即可。虽然输出流名称保留了
  `v4l2_720p` 历史名称，实际视频尺寸由 MPP 配置决定，为 3840×2160。
- 循环查看启动日志确认板端加载 `config_4k_18mbps.json`，成功拉到 3840×2160 输入；稳定窗口曾记录
  `output_fps=26.57`、`bitrate_kbps=16096.7`、`write_fail=0`。本次循环进程因会话中断已停止，日志保存为
  `runs/testsrc2/board_4k_loop_20260714.log`。

## 实测结果

- SRS 确认输入为 H.264 High、3840×2160，输入接收带宽约 18.03 Mbps。
- SRS 确认输出为 H.264 Main、3840×2160，输出发送带宽约 16.10 Mbps，当前有 2 个客户端。
- 板端 `EncodeStats`：`packet_fps=28.47`、`bitrate_kbps=16952.9`、`put_fail=0`、`out_drop=0`、队列为 0。
- 板端 `StreamStats`：`output_fps=28.54`、`bitrate_kbps=16964`、`write_fail=0`、队列为 0。
- RKNN 推理持续运行，单帧总耗时约 43～64 ms，检测结果持续输出 1～4 个目标。

## 结论

18 Mbps 配置已经生效，带宽从低码率测试提升到约 17 Mbps 编码输出；没有发现推流失败或编码队列堆积。
实际带宽略低于配置值，主要与当前约 28.5 FPS 的处理/输出速率和统计窗口有关，不是配置未生效。

播放器验证地址：`rtmp://192.168.0.168/live/v4l2_720p_result`。
