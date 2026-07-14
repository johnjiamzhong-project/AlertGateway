# 4K 帧率修正与 6 Mbps 验证记录（2026-07-13）

## 修正内容

第一轮 6 Mbps 仪表化测试发现：输入约 30 FPS，编码和 RTMP 写出也约 30 FPS，而配置目标是 15 FPS。流元数据虽然标记为 15 FPS，但实际处理节奏不一致，可能造成播放端卡顿或时间戳异常。

`PullStreamThread` 现按输入流声明帧率计算抽帧步长。对于本次 30 FPS 输入和 15 FPS 目标，`frame_step=2`，每两帧保留一帧；输入帧率未知或不高于目标时，保留原有 PTS 门控作为兜底。该修正只影响 `pull_stream`，不改变 V4L2 路径。

## 复测配置

- 输入：3840×2160 H.264 `yuvj420p`，约 30 FPS。
- 输出：3840×2160 H.264，目标 15 FPS，CBR 6000 kbps。
- 配置：`runs/testsrc2/config_testsrc2_4k_6m.json`。
- 板端：Firefly RK3588S `192.168.0.200`。
- 观测窗口：启动后约 30 秒，另抓取输出 20 秒。

## 结果

- 拉流日志确认 `source_fps=30 target_fps=15 frame_step=2`。
- 稳定窗口输入约 30.07–30.13 FPS，抽帧丢弃约 151 帧/10 秒，编码输入约 15.02 FPS。
- 稳定窗口 RTMP 写出约 15.02 FPS，实际码率约 5.92 Mbps；前一窗口约 14.97 FPS、5.98 Mbps。
- 编码平均处理耗时约 18.1 ms，`put_fail=0`、`out_drop=0`、`write_fail=0`，队列均为 0。
- 输出抓取 20 秒：3840×2160 H.264，标称 15 FPS，平均约 15.17 FPS，实际约 5.98 Mbps。
- 关闭时的 FLV header 提示仍只出现在主动退出阶段。

## 结论

帧率节奏问题已在本次 30→15 FPS 场景得到修正，6 Mbps 也已生效。下一步可以在保持该抽帧策略不变的前提下，做 6/8/12 Mbps 画质对比；若播放器仍有卡顿，再继续拆分 RTMP 时间戳和接收端缓冲问题。

原始日志：

- `runs/testsrc2/board_4k_stats_6m_20260713.log`：修正前 6 Mbps、约 30 FPS 写出。
- `runs/testsrc2/board_4k_paced2_6m_20260713.log`：修正后约 15 FPS 写出。
