# 4K 码率矩阵测试记录（2026-07-13）

测试条件保持一致：3840×2160 H.264 约 30 FPS 输入，PullStreamThread 按 `frame_step=2` 抽到 15 FPS，RKNN 检测和 MPP H.264 编码均开启，输出分辨率 3840×2160。

| 目标码率 | 输出抓取实际码率 | 稳定输出 FPS | 编码/写出失败 | 队列 |
|---:|---:|---:|---:|---:|
| 3 Mbps | 3.16 Mbps（稳定窗口约 3.22 Mbps） | 15.03 | 0 | 0 |
| 6 Mbps | 5.98 Mbps（稳定窗口约 5.92–5.98 Mbps） | 15.02 | 0 | 0 |
| 8 Mbps | 7.94 Mbps（稳定窗口约 8.04 Mbps） | 15.05 | 0 | 0 |
| 12 Mbps | 11.87 Mbps（稳定窗口约 12.03 Mbps） | 15.03 | 0 | 0 |

## 结论

- 4 个码率档位在固定抽帧策略下都能稳定维持约 15 FPS，当前瓶颈不是 MPP 码率升高导致的帧率下降。
- MPP CBR 实际码率与目标基本一致；3 Mbps 的 4K 画质风险已经通过配置确认，但本表只记录工程指标，不替代人工画质评价。
- 下一步只需对同一时间段的 3/6/8/12 Mbps 输出做人工放大对比，重点观察细小物体、文字标签、运动边缘和暗部噪声，再选择生产码率。

原始板端日志：

- `runs/testsrc2/board_4k_matrix_3000k_20260713.log`
- `runs/testsrc2/board_4k_stats_6m_20260713.log`
- `runs/testsrc2/board_4k_matrix_8000k_retry_20260713.log`
- `runs/testsrc2/board_4k_matrix_12000k_20260713.log`

## 2026-07-14 修正后的 30 FPS 12/18 Mbps 对比

早期 15 FPS 自动化报告使用了旧版 AlertGateway 二进制和旧板端配置，因此无效。该报告已由使用 AlertGateway.maxfps_20260714 和 30 FPS 配置的修正运行取代。

| Target | Measured bitrate | Output FPS | PSNR | SSIM | Write/queue errors |
|---|---:|---:|---:|---:|---|
| 12 Mbps | 12007.9 kbps | 29.99 | 28.4200 dB | 0.5735 | 0 / 0 |
| 18 Mbps | 18087.0 kbps | 30.08 | 28.4099 dB | 0.5813 | 0 / 0 |

两种设置均稳定。在这张单帧对齐图中，18 Mbps 的 SSIM 略有提升，而 PSNR 基本持平；12 Mbps 仍是更节省带宽的选项。完整产物： runs/testsrc2/quality_compare_12v18_20260714/.
