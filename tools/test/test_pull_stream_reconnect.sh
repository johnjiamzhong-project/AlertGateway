#!/usr/bin/env bash
set -euo pipefail
VIDEO="${1:-runs/input_videos/4k/VID_20260712_131410.mp4}"
SRS_URL="${2:-rtmp://192.168.0.168/live/testsrc2}"
FIRST_SEC="${3:-18}"
GAP_SEC="${4:-20}"
SECOND_SEC="${5:-60}"
LOG="${6:-/tmp/pull_reconnect_push.log}"
SRS_API="${SRS_API:-http://192.168.0.168:1985/api/v1/streams/}"
SRS_HOST="${SRS_HOST:-192.168.0.168}"
SRS_RTMP_PORT="${SRS_RTMP_PORT:-1935}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -f "$VIDEO" ]]; then echo "Video not found: $VIDEO" >&2; exit 2; fi
bash "$SCRIPT_DIR/check_srs_manual.sh" "$SRS_API" "$SRS_HOST" "$SRS_RTMP_PORT" \
  "${SRS_PREFLIGHT_OUTPUT:-/tmp/alertgateway_srs_before.json}" \
  || { echo "SRS 未就绪，未启动重连发布端。" >&2; exit 1; }
: > "$LOG"
ARGS=(-loglevel error -re -stream_loop -1 -i "$VIDEO" -map 0:v:0 -c copy -f flv "$SRS_URL")
start_push() {
  ffmpeg "${ARGS[@]}" >>"$LOG" 2>&1 &
  PUSH_PID=$!
  echo "PUBLISHER_STARTED pid=$PUSH_PID" | tee -a "$LOG"
}
stop_push_hard() {
  if kill -0 "$PUSH_PID" 2>/dev/null; then
    kill -KILL "$PUSH_PID" 2>/dev/null || true
    wait "$PUSH_PID" 2>/dev/null || true
    echo "PUBLISHER_KILLED pid=$PUSH_PID" | tee -a "$LOG"
  fi
}
start_push
sleep "$FIRST_SEC"
stop_push_hard
sleep "$GAP_SEC"
start_push
sleep "$SECOND_SEC"
stop_push_hard
echo "RECONNECT_PUSH_TEST_DONE" | tee -a "$LOG"
