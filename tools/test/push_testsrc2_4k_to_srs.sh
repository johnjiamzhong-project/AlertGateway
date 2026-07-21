#!/usr/bin/env bash
set -euo pipefail
VIDEO="${1:-runs/testsrc2/testsrc2_4k_60s.mp4}"
SRS_URL="${2:-rtmp://192.168.0.168/live/testsrc2}"
DURATION="${3:-0}"
SRS_API="${SRS_API:-http://192.168.0.168:1985/api/v1/streams/}"
SRS_HOST="${SRS_HOST:-192.168.0.168}"
SRS_RTMP_PORT="${SRS_RTMP_PORT:-1935}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -f "$VIDEO" ]]; then echo "Video not found: $VIDEO" >&2; exit 2; fi
if [[ "${SRS_PREFLIGHT_ALREADY_CHECKED:-0}" != 1 ]]; then
  bash "$SCRIPT_DIR/check_srs_manual.sh" "$SRS_API" "$SRS_HOST" "$SRS_RTMP_PORT" \
    "${SRS_PREFLIGHT_OUTPUT:-/tmp/alertgateway_srs_before.json}" \
    || { echo "SRS 未就绪，未启动发布端。" >&2; exit 1; }
fi
readarray -t INFO < <(ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate -of csv=p=0 "$VIDEO")
echo "Input: $VIDEO (${INFO[*]})"
echo "Push:  $SRS_URL"
ARGS=(-re -stream_loop -1 -i "$VIDEO" -map 0:v:0 -c copy -f flv "$SRS_URL")
if [[ "$DURATION" != "0" ]]; then
  set +e
  timeout --preserve-status --signal=INT "${DURATION}s" ffmpeg "${ARGS[@]}"
  RC=$?
  set -e
  if [[ $RC -eq 124 || $RC -eq 130 ]]; then exit 0; fi
  exit $RC
else
  exec ffmpeg "${ARGS[@]}"
fi
