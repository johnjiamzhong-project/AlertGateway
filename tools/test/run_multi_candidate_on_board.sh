#!/usr/bin/env bash
# Run the in-process dual-channel PoC: two host publishers -> SRS -> one AlertGateway process.
# Each channel publishes its inference result to its own fixed-template RTMP output.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

VIDEO_A="${VIDEO_A:-runs/input_videos/4k/VID_20260712_131214.mp4}"
VIDEO_B="${VIDEO_B:-runs/input_videos/4k/VID_20260712_131410.mp4}"
RUN_SEC="${RUN_SEC:-180}"
BOARD_HOST="${BOARD_HOST:-firefly@192.168.0.200}"
BOARD_DIR="${BOARD_DIR:-~/AlertGateway}"
SRS_API="${SRS_API:-http://192.168.0.168:1985/api/v1/streams/}"
SRS_HOST="${SRS_HOST:-192.168.0.168}"
SRS_RTMP_PORT="${SRS_RTMP_PORT:-1935}"
INPUT_A_URL="rtmp://192.168.0.168/live/dual_a"
INPUT_B_URL="rtmp://192.168.0.168/live/dual_b"
RESULT_A_URL="rtmp://192.168.0.168/live/alertgateway_channel_1"
RESULT_B_URL="rtmp://192.168.0.168/live/alertgateway_channel_2"
RUN_ID="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="${LOG_DIR:-/tmp/alertgateway_multi_${RUN_ID}}"
TEMPLATE_CONFIG="config/config_multi_1080p_candidate.json"
LOCAL_CONFIG="$LOG_DIR/config_multi.json"
REMOTE_CONFIG="/tmp/alertgateway_multi_${RUN_ID}.json"
REMOTE_LOG="/tmp/alertgateway_multi_${RUN_ID}.log"

PUBLISH_A_PID=""
PUBLISH_B_PID=""
BOARD_SSH_PID=""
BOARD_STARTED=0

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
sys.exit(0 if any(
    item.get("name") == sys.argv[2]
    and (item.get("active") or item.get("publish", {}).get("active"))
    for item in streams
) else 1)
PY
}

wait_stream() {
  local name="$1" timeout_sec="$2" output="$3"
  local deadline=$((SECONDS + timeout_sec))
  while (( SECONDS < deadline )); do
    if fetch_srs "$output" && stream_active "$output" "$name"; then
      return 0
    fi
    # API 不可达时，直接从 RTMP 读取流元数据，避免把监控通道故障误判成流故障。
    if timeout 8 ffprobe -v error -rw_timeout 5000000 \
      -show_entries stream=codec_name,width,height -of default=noprint_wrappers=1 \
      "rtmp://192.168.0.168/live/${name}" >"${output%.json}.probe.txt" 2>/dev/null; then
      return 0
    fi
    sleep 2
  done
  return 1
}

stop_local() {
  local pid="$1"
  if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
    kill -INT "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
}

collect_artifacts() {
  ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
    "test -f '$REMOTE_LOG' && cat '$REMOTE_LOG'" >"$LOG_DIR/board.log" 2>/dev/null || true
}

cleanup() {
  local code=$?
  trap - EXIT INT TERM
  stop_local "$PUBLISH_A_PID"
  stop_local "$PUBLISH_B_PID"
  if (( BOARD_STARTED )); then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
      "pkill -INT -f 'AlertGateway ${REMOTE_CONFIG}' || true" >/dev/null 2>&1 || true
    if [[ -n "$BOARD_SSH_PID" ]] && kill -0 "$BOARD_SSH_PID" 2>/dev/null; then
      wait "$BOARD_SSH_PID" 2>/dev/null || true
    fi
  fi
  collect_artifacts
  ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
    "rm -f '$REMOTE_CONFIG'" >/dev/null 2>&1 || true
  if (( code != 0 )); then
    tail -n 40 "$LOG_DIR/publisher_a.log" 2>/dev/null || true
    tail -n 40 "$LOG_DIR/publisher_b.log" 2>/dev/null || true
    tail -n 80 "$LOG_DIR/board.log" 2>/dev/null || true
  fi
  exit "$code"
}
trap cleanup EXIT INT TERM

[[ "$RUN_SEC" =~ ^[1-9][0-9]*$ ]] || fail "运行秒数必须为正整数"
[[ -f "$VIDEO_A" && -f "$VIDEO_B" ]] || fail "找不到双路输入视频"
[[ -f "$TEMPLATE_CONFIG" ]] || fail "找不到多路配置模板：$TEMPLATE_CONFIG"
for command in ffmpeg ffprobe curl python3 ssh scp timeout; do
  command -v "$command" >/dev/null || fail "缺少命令：$command"
done

python3 - "$TEMPLATE_CONFIG" "$LOCAL_CONFIG" <<'PY'
import json
import sys

template, output = sys.argv[1:]
with open(template, encoding="utf-8") as handle:
    config = json.load(handle)
with open(output, "w", encoding="utf-8") as handle:
    json.dump(config, handle, ensure_ascii=False, indent=2)
    handle.write("\n")
PY

log "=== 单进程双路 ChannelPipeline PoC ==="
log "A：$VIDEO_A -> $INPUT_A_URL -> $RESULT_A_URL"
log "B：$VIDEO_B -> $INPUT_B_URL -> $RESULT_B_URL"
log "日志目录：$LOG_DIR"

log "检查 SRS 前置条件；SRS 只允许手动启动，测试脚本不会启动或重启它。"
bash tools/test/check_srs_manual.sh "$SRS_API" "$SRS_HOST" "$SRS_RTMP_PORT" \
  "$LOG_DIR/srs_before.json" || fail "SRS 未就绪，未启动本次测试。"
ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "cd $BOARD_DIR && test -x AlertGateway && test -f model/yolov8s_4k_photos_head_20e_20260718_finaltest_int8.rknn" \
  || fail "板端缺少新二进制或候选 RKNN"
if ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "pgrep -af 'AlertGateway' | grep -v 'pgrep -af'"; then
  fail "板端已有 AlertGateway 在运行，本脚本不会中断已有进程"
fi
scp "$LOCAL_CONFIG" "$BOARD_HOST:$REMOTE_CONFIG" >/dev/null

timeout --preserve-status --signal=INT "$((RUN_SEC + 45))s" \
  ffmpeg -hide_banner -loglevel info -re -stream_loop -1 -i "$VIDEO_A" -map 0:v:0 \
  -vf "scale=1920:1080:flags=fast_bilinear" -r 30 -c:v libx264 -preset veryfast \
  -tune zerolatency -pix_fmt yuv420p -g 60 -f flv "$INPUT_A_URL" \
  >"$LOG_DIR/publisher_a.log" 2>&1 &
PUBLISH_A_PID=$!
timeout --preserve-status --signal=INT "$((RUN_SEC + 45))s" \
  ffmpeg -hide_banner -loglevel info -re -stream_loop -1 -i "$VIDEO_B" -map 0:v:0 \
  -vf "scale=1920:1080:flags=fast_bilinear" -r 30 -c:v libx264 -preset veryfast \
  -tune zerolatency -pix_fmt yuv420p -g 60 -f flv "$INPUT_B_URL" \
  >"$LOG_DIR/publisher_b.log" 2>&1 &
PUBLISH_B_PID=$!

wait_stream "dual_a" 30 "$LOG_DIR/srs_input_a.json" || fail "A 路输入未 active"
wait_stream "dual_b" 30 "$LOG_DIR/srs_input_b.json" || fail "B 路输入未 active"

ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "cd $BOARD_DIR && timeout --preserve-status --signal=INT --kill-after=5s \
   '${RUN_SEC}s' ./AlertGateway '$REMOTE_CONFIG' >'$REMOTE_LOG' 2>&1" &
BOARD_SSH_PID=$!
BOARD_STARTED=1

wait_stream "alertgateway_channel_1" 35 "$LOG_DIR/srs_result_a.json" || fail "A 路结果流未 active"
wait_stream "alertgateway_channel_2" 35 "$LOG_DIR/srs_result_b.json" || fail "B 路结果流未 active"
log "A 路可观看：$RESULT_A_URL"
log "B 路可观看：$RESULT_B_URL"
wait "$BOARD_SSH_PID" || fail "板端多路进程异常退出"
BOARD_STARTED=0
fetch_srs "$LOG_DIR/srs_final.json" || true
log "=== 运行完成，检查 $LOG_DIR/board.log、srs_result_a.json 与 srs_result_b.json ==="
