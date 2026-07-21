#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

# 统一测试入口：通过注释切换测试模式，不增加命令行参数。
# 下面只能保留一个未注释的 exec；切换时注释当前模式，再取消目标模式的注释。

# 单路 4K + ROI/Tiling 验证（当前默认，先验证局部推理和坐标还原）：
CONFIG=config/config_4k_roi_candidate_20260720.json exec tools/test/run_4k_candidate_on_board.sh \
  runs/input_videos/4k/VID_20260712_131214.mp4 120

# 双路 1080p 并发 PoC：
# exec tools/test/run_multi_candidate_on_board.sh

# 后续三路或更多路测试：新增对应脚本后，在这里替换/取消注释，例如：
# exec tools/test/run_multi_3plus_candidate_on_board.sh
