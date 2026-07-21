#!/usr/bin/env bash
# Four 4K publishers -> manually started SRS -> decode to 1080p -> one global-Scheduler process.
# This script never starts, restarts, or stops SRS.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"
export NO_PROXY="192.168.0.168,192.168.0.200${NO_PROXY:+,$NO_PROXY}"
export no_proxy="$NO_PROXY"

VIDEO_A="${VIDEO_A:-runs/input_videos/4k/VID_20260712_131214.mp4}"
VIDEO_B="${VIDEO_B:-runs/input_videos/4k/VID_20260712_131410.mp4}"
VIDEO_C="${VIDEO_C:-$VIDEO_A}"
VIDEO_D="${VIDEO_D:-$VIDEO_B}"
RUN_SEC="${RUN_SEC:-120}"
BOARD_HOST="${BOARD_HOST:-firefly@192.168.0.200}"
BOARD_DIR="${BOARD_DIR:-~/AlertGateway}"
SRS_API="${SRS_API:-http://192.168.0.168:1985/api/v1/streams/}"
SRS_HOST="${SRS_HOST:-192.168.0.168}"
SRS_RTMP_PORT="${SRS_RTMP_PORT:-1935}"
CONFIG="${CONFIG:-config/config_multi_4ch_scheduler_4k_to1080_candidate.json}"
RUN_ID="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="${LOG_DIR:-/tmp/alertgateway_4k_4channel_${RUN_ID}}"
REMOTE_BINARY="/tmp/AlertGateway.4k_4channel_${RUN_ID}"
REMOTE_CONFIG="/tmp/alertgateway_4k_4channel_${RUN_ID}.json"
REMOTE_LOG="/tmp/alertgateway_4k_4channel_${RUN_ID}.log"

INPUT_URLS=(
  "rtmp://192.168.0.168/live/alertgateway_4k_source_a"
  "rtmp://192.168.0.168/live/alertgateway_4k_source_b"
  "rtmp://192.168.0.168/live/alertgateway_4k_source_c"
  "rtmp://192.168.0.168/live/alertgateway_4k_source_d"
)
RESULT_URLS=(
  "rtmp://192.168.0.168/live/alertgateway_channel_1"
  "rtmp://192.168.0.168/live/alertgateway_channel_2"
  "rtmp://192.168.0.168/live/alertgateway_channel_3"
  "rtmp://192.168.0.168/live/alertgateway_channel_4"
)
STREAM_NAMES=(alertgateway_4k_source_a alertgateway_4k_source_b alertgateway_4k_source_c alertgateway_4k_source_d)
VIDEOS=("$VIDEO_A" "$VIDEO_B" "$VIDEO_C" "$VIDEO_D")
PUBLISH_PIDS=()
BOARD_SSH_PID=""
BOARD_STARTED=0
SRS_WATCH_PID=""

mkdir -p "$LOG_DIR"
log() { printf '[%(%F %T)T] %s\n' -1 "$*"; }
fail() { log "ERROR: $*" >&2; exit 1; }

fetch_srs() {
  curl --connect-timeout 3 --max-time 5 --fail --silent --show-error "$SRS_API" >"$1"
}

stream_active() {
  python3 - "$1" "$2" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as handle:
    data = json.load(handle)
streams = data if isinstance(data, list) else data.get("streams", [])
sys.exit(0 if any(item.get("name") == sys.argv[2] and
                  (item.get("active") or item.get("publish", {}).get("active"))
                  for item in streams) else 1)
PY
}

wait_stream() {
  local name="$1" timeout_sec="$2" output="$3"
  local deadline=$((SECONDS + timeout_sec))
  while (( SECONDS < deadline )); do
    if fetch_srs "$output" && stream_active "$output" "$name"; then return 0; fi
    sleep 2
  done
  return 1
}

stop_publishers() {
  local pid
  for pid in "${PUBLISH_PIDS[@]}"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill -INT "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  PUBLISH_PIDS=()
}

start_srs_watch() {
  (
    while true; do
      if fetch_srs "$LOG_DIR/srs_watch_current.json"; then
        printf '%(%FT%T%z)T\t' -1 >>"$LOG_DIR/srs_watch.jsonl"
        tr -d '\n' <"$LOG_DIR/srs_watch_current.json" >>"$LOG_DIR/srs_watch.jsonl"
        printf '\n' >>"$LOG_DIR/srs_watch.jsonl"
      fi
      sleep 10
    done
  ) &
  SRS_WATCH_PID=$!
}

stop_srs_watch() {
  if [[ -n "$SRS_WATCH_PID" ]] && kill -0 "$SRS_WATCH_PID" 2>/dev/null; then
    kill "$SRS_WATCH_PID" 2>/dev/null || true
    wait "$SRS_WATCH_PID" 2>/dev/null || true
  fi
  SRS_WATCH_PID=""
}

collect_board_log() {
  ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
    "test -f '$REMOTE_LOG' && cat '$REMOTE_LOG'" >"$LOG_DIR/board.log" 2>/dev/null || true
}

cleanup() {
  local code=$?
  trap - EXIT INT TERM
  stop_srs_watch
  stop_publishers
  if (( BOARD_STARTED )); then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
      "pkill -INT -f '$REMOTE_BINARY $REMOTE_CONFIG' || true" >/dev/null 2>&1 || true
    if [[ -n "$BOARD_SSH_PID" ]] && kill -0 "$BOARD_SSH_PID" 2>/dev/null; then
      wait "$BOARD_SSH_PID" 2>/dev/null || true
    fi
  fi
  collect_board_log
  ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
    "rm -f '$REMOTE_BINARY' '$REMOTE_CONFIG'" >/dev/null 2>&1 || true
  if (( code != 0 )); then
    tail -n 40 "$LOG_DIR"/publisher_*.log 2>/dev/null || true
    tail -n 100 "$LOG_DIR/board.log" 2>/dev/null || true
    log "调试文件保留在：$LOG_DIR"
  fi
  exit "$code"
}
trap cleanup EXIT INT TERM

[[ "$RUN_SEC" =~ ^[1-9][0-9]*$ ]] || fail "运行秒数必须为正整数"
[[ -f build/AlertGateway ]] || fail "找不到本地 build/AlertGateway，请先交叉编译"
[[ -f "$CONFIG" ]] || fail "找不到四路 4K 配置：$CONFIG"
for video in "${VIDEOS[@]}"; do [[ -f "$video" ]] || fail "找不到输入视频：$video"; done
for command in ffmpeg ffprobe curl python3 ssh scp timeout; do
  command -v "$command" >/dev/null || fail "缺少命令：$command"
done

for video in "${VIDEOS[@]}"; do
  ffprobe -v error -select_streams v:0 \
    -show_entries stream=codec_name,width,height,r_frame_rate \
    -of default=noprint_wrappers=1 "$video" || fail "无法读取输入视频：$video"
done

log "=== 四路 4K 输入 -> 1080p 处理/推理 NPU Scheduler 测试 ==="
log "配置：$CONFIG，4K 解码后降为 1920x1080；运行 ${RUN_SEC} 秒；ROI/Tiling/Thumbnail 全部关闭"
log "日志目录：$LOG_DIR"
for index in 0 1 2 3; do
  log "第 $((index + 1)) 路：${VIDEOS[$index]} -> ${INPUT_URLS[$index]} -> ${RESULT_URLS[$index]}"
done

log "检查 SRS 前置条件；SRS 只允许手动启动，测试脚本不会启动或重启它。"
bash tools/test/check_srs_manual.sh "$SRS_API" "$SRS_HOST" "$SRS_RTMP_PORT" \
  "$LOG_DIR/srs_before.json" || fail "SRS 未就绪，未启动本次测试。"

ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "cd $BOARD_DIR && test -f model/yolov8s_4k_photos_head_20e_20260718_finaltest_int8.rknn" \
  || fail "板端缺少候选 RKNN 或 SSH 不可达：$BOARD_HOST"
if ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "pgrep -af 'AlertGateway' | grep -v 'pgrep -af'"; then
  fail "板端已有 AlertGateway 在运行，本脚本不会中断已有进程"
fi

log "部署隔离候选二进制和四路 4K 配置。"
scp build/AlertGateway "$BOARD_HOST:$REMOTE_BINARY" >/dev/null
scp "$CONFIG" "$BOARD_HOST:$REMOTE_CONFIG" >/dev/null

for index in 0 1 2 3; do
  timeout --preserve-status --signal=INT "$((RUN_SEC + 45))s" \
    ffmpeg -hide_banner -loglevel info -re -stream_loop -1 -i "${VIDEOS[$index]}" \
    -map 0:v:0 -c:v copy -f flv "${INPUT_URLS[$index]}" \
    >"$LOG_DIR/publisher_$((index + 1)).log" 2>&1 &
  PUBLISH_PIDS+=("$!")
done

for index in 0 1 2 3; do
  wait_stream "${STREAM_NAMES[$index]}" 35 "$LOG_DIR/srs_input_$((index + 1)).json" \
    || fail "第 $((index + 1)) 路 4K 输入未 active"
done

log "四路 4K 输入均已 active，启动单进程全局 NPU Scheduler（处理分辨率 1920x1080）。"
ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "cd $BOARD_DIR && timeout --preserve-status --signal=INT --kill-after=5s \
   '${RUN_SEC}s' '$REMOTE_BINARY' '$REMOTE_CONFIG' >'$REMOTE_LOG' 2>&1" &
BOARD_SSH_PID=$!
BOARD_STARTED=1

for channel in 1 2 3 4; do
  wait_stream "alertgateway_channel_${channel}" 45 "$LOG_DIR/srs_result_${channel}.json" \
    || fail "第 ${channel} 路 4K 结果流未 active"
done
for url in "${RESULT_URLS[@]}"; do log "结果流：$url"; done

start_srs_watch
log "已开始每 10 秒保存 SRS 快照：$LOG_DIR/srs_watch.jsonl"
log "运行 ${RUN_SEC} 秒；按 Ctrl+C 可提前结束，脚本只清理本次启动的进程。"
wait "$BOARD_SSH_PID" || fail "板端四路 4K Scheduler 异常退出"
BOARD_STARTED=0
stop_srs_watch
stop_publishers
fetch_srs "$LOG_DIR/srs_final.json" || true
collect_board_log
log "=== 四路 4K 测试完成 ==="
log "板端日志：$LOG_DIR/board.log"
log "SRS 快照：$LOG_DIR/srs_watch.jsonl"
grep -E '\[NpuScheduler\]|EncodeStats|StreamStats|PullStats|RGA|ERROR|Error|failed|Failed|Done' \
  "$LOG_DIR/board.log" | tail -n 80 || true
