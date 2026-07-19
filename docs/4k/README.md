# 4K 文档索引

本索引汇总 4K 拉流设计、问题复现、帧率修复、码率验证和自动化对比记录。 所有 4K Markdown 文档集中在本目录，测试产物和脚本仍保留在原有的 runs/ 和 tools/ 目录。

## 架构与方案

- [4K 拉流图像处理方案](4k_pull_stream_image_processing_plan.md): 单路 4K 流水线设计、ROI、切片和图像处理扩展点.
- [4K 准确率微调与 INT8 校准](4k_accuracy_finetune_and_int8_calibration.md): 从已有 4K 视频按 1 FPS 抽帧、标注、按视频划分数据集、WSL 微调、量化校准和板端验收的执行规范。
- [4K 架构图](../architecture/alertgateway_architecture.svg): 输入源、SRS、RK3588 推理、MPP 编码和输出流程.

## 推流、帧率与码率验证

- [4K 30 FPS 码率验证](4k_30fps_bitrate_validation_20260714.md): 当前代码修改、6/12/18 Mbps 预设、修正后的板端指标和产物路径.
- [12 与 18 Mbps 画质报告](../../runs/testsrc2/quality_compare_12v18_20260714/README.md): 对齐帧、PSNR/SSIM、并排对比图、FLV 录制和板端日志.

## 自动化与测试文件

- ../../tools/test/run_compare_12v18_20260714.sh: 使用当前 30 FPS 二进制执行修正后的 12/18 Mbps 板端对比.
- ../../tools/test/evaluate_quality_compare_12v18.py: 对齐帧并计算 PSNR/SSIM.
- ../../runs/testsrc2/: 4K 配置、源视频、板端日志和抓取输出.

## 当前已验证结论

- 12 Mbps: 12007.9 kbps, 29.99 FPS, PSNR 28.4200 dB, SSIM 0.5735.
- 18 Mbps: 18087.0 kbps, 30.08 FPS, PSNR 28.4099 dB, SSIM 0.5813.
- 两组测试写入失败数和队列深度均为 0。单帧对齐样本中 18 Mbps 的 SSIM 略有优势；12 Mbps 仍是更节省带宽的选项。
