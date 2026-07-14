# 4K 拉流 6 Mbps 验收 — 2026-07-13

## 范围

This acceptance uses the dedicated 4K `pull_stream` test configuration. The existing V4L2 production configuration was not changed.

## 板端运行

- Board: `192.168.0.200`
- Input: `3840x2160`, H.264 `yuvj420p`, about 30 FPS
- Target output: `3840x2160`, 15 FPS, 6000 kbps
- Duration captured: 51.933 seconds
- Output bitrate: 6026 kbps
- Output nominal/average frame rate: 15 / 15.17 FPS

稳定窗口显示 14.86–15.17 FPS and 5.91–6.08 Mbps. Encode and stream queues stayed at zero, with zero `put_fail`, `out_drop`, and `write_fail` counters. One transient queue value of 1 appeared during startup and returned to zero.

由于早期验证曾使用错误的源路径，板端生产 V4L2 配置已恢复。4K 测试配置仍是已验证的 6 Mbps 候选方案；未修改生产 V4L2 码率。
