#!/usr/bin/env bash
set -euo pipefail
VIDEO="${1:-runs/input_videos/4k/VID_20260712_131410.mp4}"
SRS_URL="${2:-rtmp://192.168.0.168/live/testsrc2}"
FIRST_SEC="${3:-18}"
GAP_SEC="${4:-20}"
SECOND_SEC="${5:-60}"
LOG="${6:-/tmp/pull_reconnect_push.log}"
if [[ ! -f "$VIDEO" ]]; then echo "Video not found: $VIDEO" >&2; exit 2; fi
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
