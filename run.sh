#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

exec tools/test/run_4k_candidate_on_board.sh \
  runs/input_videos/4k/VID_20260712_131214.mp4 180
