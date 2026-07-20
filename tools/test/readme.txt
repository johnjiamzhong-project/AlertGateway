4K 拉流与板端测试脚本
======================

本目录脚本用于测试链路：

本机视频 -> SRS live/testsrc2 -> RK3588 拉流、RKNN 推理、MPP 编码
-> SRS rtmp://192.168.0.168/live/alertgateway

当前推荐入口：

  ../../run.sh

它等价于：

  ./run_4k_candidate_on_board.sh \
    ../../runs/input_videos/4k/VID_20260712_131214.mp4 180

run_4k_candidate_on_board.sh 会在启动前检查视频、SRS API、SSH、板端候选文件和
已有 AlertGateway 进程；确认输入流 active 后才启动板端。若发布、拉流、RKNN、编码或
结果回推失败，会打印发布端/板端日志末尾，并将完整调试资料保存到：

  /tmp/alertgateway_4k_时间戳/

结果流 active 后，脚本还会每 10 秒记录一份 SRS streams API 快照到同一目录的
`srs_watch.jsonl`。观看 8/18 Mbps A/B 时，可用其中 `alertgateway` 的 `clients`、
`kbps.recv_30s` 和 `kbps.send_30s` 证明播放器是否持续接收完整码率；它只记录证据，
不改变 SRS、播放器或板端配置。

其他脚本：

- push_testsrc2_4k_to_srs.sh：只将指定视频循环发布到 SRS 输入流，不启动板端程序。
- test_pull_stream_reconnect.sh：模拟发布端中断和恢复，验证板端自动重连。
- run_4k_sync_compare_6v8_20260713.sh、run_compare_12v18_20260714.sh：历史对比脚本，
  使用前必须核对其中的配置、输出地址和进程清理行为，不能作为当前常规入口。
- capture_mqtt.py、evaluate_quality_compare_12v18.py：辅助采集与离线质量分析工具。

运行前要求：

1. Windows SRS 已启动，RTMP 1935 和 HTTP API 1985 可访问。
2. 板端地址为 firefly@192.168.0.200，已部署 AlertGateway、
   config_4k_candidate_20260719.json、config_4k_18mbps.json、config_4k_8mbps.json 和对应候选 RKNN。
   当前候选默认使用已验收的 18 Mbps；网络受限时可临时改用 8 Mbps 基础配置。
3. 不要同时手动启动另一个 AlertGateway；推荐脚本发现已有进程会终止本次启动，避免误中断测试。
