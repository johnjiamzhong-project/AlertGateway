AlertGateway 工具目录
======================

本目录存放开发、数据处理、模型转换、性能分析和板端辅助脚本；它们不属于
AlertGateway 主程序的运行时依赖。执行前请先确认所用数据集、模型和板端地址。

目录说明：

- analysis/：检测框运动、平滑策略的离线分析与回放工具。
- benchmark/：RKNN、后处理和图像处理的基准/烟雾测试程序与评估脚本。
- dataset/：数据筛选、标注辅助、训练、导出、量化和离线精度评估工具。
- performance/：RK3588 CPU/NPU/DMC performance governor 的部署脚本与 systemd 服务。
- test/：SRS、4K 拉流、板端推理和推流链路的测试脚本，见 test/readme.txt。

常用入口：

- 根目录 run.sh：统一测试入口，当前默认单进程双路 1080p 并发 PoC；通过注释切换单路、双路或未来多路脚本。
- test/run_multi_candidate_on_board.sh：单进程 `channels[]` 双路候选验证入口。
- test/run_dual_candidate_on_board.sh：两个独立进程的历史双路隔离验证入口。
- test/run_4k_candidate_on_board.sh：单路 4K 候选验证入口，仍可独立使用。

注意：

1. 当前板端为 firefly@192.168.0.200。
2. 所有新的/活动的推理结果流必须固定推送到：
   rtmp://192.168.0.168/live/alertgateway
3. 4K 专用候选只用于 pull_stream 验证，不覆盖 V4L2 生产模型或 config/config.json。
4. 训练数据、模型权重和板端文件通常在仓库外或板端目录中；不要假定仓库内已有完整产物。
