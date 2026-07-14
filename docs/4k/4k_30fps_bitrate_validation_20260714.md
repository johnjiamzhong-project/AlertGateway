# 4K 30 FPS 与码率验证记录 (2026-07-14)

## 范围

本文记录 4K 拉流修改：使用完整源帧作为编码输入、编码器和推流队列采用非阻塞方式、推理采用最新帧策略，以及可配置的 6/12/18 Mbps CBR 输出。

## 代码修改

- src/capture/PullStreamThread.cpp: removed pre-encode 30-to-15 FPS pacing; encoder push is non-blocking; PullStats records source, encoder, inference and drop counters.
- src/encode/EncodeThread.cpp and src/encode/EncodeThread.hpp: non-blocking encoded-packet output; EncodeStats records FPS, bitrate, processing time and drops; 4K detection labels use 2x glyph scaling.
- src/stream/StreamThread.cpp: StreamStats records output FPS, bitrate, write failures and queue depth.

## 配置

| Preset | File | Target |
|---|---|---:|
| 6 Mbps | runs/testsrc2/config_testsrc2_4k_6m.json | 6000 kbps |
| 12 Mbps | runs/testsrc2/config_testsrc2_4k_12m.json | 12000 kbps |
| 18 Mbps | runs/testsrc2/config_testsrc2_4k_18m.json | 18000 kbps |

All three test presets use 3840x2160 at 30 FPS. The production V4L2 config/config.json was not changed.

## 修正后的 12/18 Mbps 结果

| Metric | 12 Mbps | 18 Mbps |
|---|---:|---:|
| Measured bitrate | 12007.9 kbps | 18087.0 kbps |
| Output FPS | 29.99 | 30.08 |
| PSNR | 28.4200 dB | 28.4099 dB |
| SSIM | 0.5735 | 0.5813 |
| Write failures | 0 | 0 |
| Queue depth | 0 | 0 |

两种设置均稳定。单帧对齐样本显示 18 Mbps 的 SSIM 略有优势，而 PSNR 基本持平；12 Mbps 仍是更节省带宽的选项。

## 测试脚本与产物

- tools/test/run_compare_12v18_20260714.sh: 执行两组板端测试并抓取 RTMP 输出.
- tools/test/evaluate_quality_compare_12v18.py: 对齐帧、计算 PSNR/SSIM，并生成并排对比图和报告.
- runs/testsrc2/quality_compare_12v18_20260714/: 输出 FLV 文件、板端日志、对齐帧、对比图和 README.
- docs/4k/4k_bitrate_matrix_20260713.md: 历史 3/6/8/12 Mbps 矩阵及修正后的 30 FPS 12/18 Mbps 结果.

注意：早期报告使用了旧二进制和 15 FPS 板端配置，已由本文记录的修正运行结果取代。
