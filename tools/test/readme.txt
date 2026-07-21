4K 拉流与板端测试脚本
======================

本目录脚本用于测试链路：

本机视频 -> SRS live/testsrc2 -> RK3588 拉流、RKNN 推理、MPP 编码
-> SRS rtmp://192.168.0.168/live/alertgateway_channel_{channel_id}

当前推荐入口：

  ../../run.sh

`run.sh` 是统一入口，当前通过未注释的 `exec` 选择单进程双路脚本；单路和未来多路测试只需在
`run.sh` 中注释/取消注释对应命令，不使用命令行参数。当前等价于：

  ./run_multi_candidate_on_board.sh

默认启动两路 1920x1080@30 FPS：A 路输入为 `live/dual_a`，结果推送固定地址
`rtmp://192.168.0.168/live/alertgateway_channel_1`；B 路输入为 `live/dual_b`，结果推送至
`rtmp://192.168.0.168/live/alertgateway_channel_2`。两路 MQTT topic 分别为 `desk/dual/a` 和 `desk/dual/b`。

该脚本会把 `config/config_multi_1080p_candidate.json` 复制为本次临时配置，并在一个 AlertGateway 进程内创建
`channel_a` 和 `channel_b`；尚未进行板端实流验收。

所有主要测试入口都会先执行 SRS 手动启动前置检查：同时确认 HTTP API `1985` 和 RTMP `1935` 可访问。
SRS 未就绪时脚本会暂停，提示用户在 Windows 上手动启动唯一的 `console.conf` 实例；用户按 Enter
后重新检查，输入 `q` 退出。测试脚本绝不会启动、重启或停止 SRS，也不会在 SRS 未确认前启动 FFmpeg
发布端、板端 AlertGateway 或其他测试进程。

run_multi_candidate_on_board.sh 还会在启动前检查两段视频、SSH、板端候选文件和
已有 AlertGateway 进程；确认两路输入 active 后才启动一个多通道路由进程。若发布、拉流、RKNN、编码或
结果回推失败，会打印两路发布端/板端日志末尾，并将完整调试资料保存到：

  /tmp/alertgateway_multi_时间戳/

退出时脚本会同步收集板端的通道路由日志和 B 路媒体信息。它只记录证据，
不改变 SRS、播放器或生产配置。

其他脚本：

- push_testsrc2_4k_to_srs.sh：只将指定视频循环发布到 SRS 输入流，不启动板端程序。
- run_multi_candidate_on_board.sh：启动双路输入和一个 `channels[]` 配置；只允许 A 路使用固定结果 RTMP。
- run_dual_candidate_on_board.sh：启动双路输入和两个独立 AlertGateway 进程，作为隔离对比入口。
- test_pull_stream_reconnect.sh：模拟发布端中断和恢复，验证板端自动重连。
- run_4k_sync_compare_6v8_20260713.sh、run_compare_12v18_20260714.sh：历史对比脚本，
  使用前必须核对其中的配置、输出地址和进程清理行为，不能作为当前常规入口。
- capture_mqtt.py、evaluate_quality_compare_12v18.py：辅助采集与离线质量分析工具。

运行前要求：

1. 测试时由用户手动启动 Windows SRS，固定使用 `console.conf`；不要同时启动 `srs-live.bat`。
   如果保留 Windows 用户 Startup 中的 `SRS AutoStart.cmd`，必须确保测试时不会再手动启动第二个实例。
   要求 RTMP 1935 和 HTTP API 1985 可访问；脚本会在前置阶段暂停等待确认。
2. 板端地址为 firefly@192.168.0.200，已部署 AlertGateway、
   config_4k_candidate_20260719.json、config_4k_18mbps.json、config_4k_8mbps.json 和对应候选 RKNN。
   当前候选默认使用已验收的 18 Mbps；网络受限时可临时改用 8 Mbps 基础配置。
3. 不要同时手动启动另一个 AlertGateway；推荐脚本发现已有进程会终止本次启动，避免误中断测试。
