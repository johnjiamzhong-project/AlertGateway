#!/usr/bin/env bash
# 4K source -> MPP decode/NV12 2:1 resize to 1080p -> four result streams.
# SRS must be started manually; the delegated runner only checks it.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"
CONFIG="${CONFIG:-config/config_multi_4ch_scheduler_4k_to1080_candidate.json}"
export CONFIG
exec ./run_4k_4channel.sh "$@"
