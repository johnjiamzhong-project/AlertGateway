#!/usr/bin/env bash
# Run the first dual-stream PoC: two host publishers -> SRS -> two RK3588 processes.
# Route A is the only live result stream; route B is written to a board-local FLV.
# This script uses temporary complete configs and never changes the production entry.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

VIDEO_A="${VIDEO_A:-runs/input_videos/4k/VID_20260712_131214.mp4}"
VIDEO_B="${VIDEO_B:-runs/input_videos/4k/VID_20260712_131410.mp4}"
RUN_SEC="${RUN_SEC:-180}"
BOARD_HOST="${BOARD_HOST:-firefly@192.168.0.200}"
BOARD_DIR="${BOARD_DIR:-~/AlertGateway}"
INPUT_A_URL="${INPUT_A_URL:-rtmp://192.168.0.168/live/dual_a}"
INPUT_B_URL="${INPUT_B_URL:-rtmp://192.168.0.168/live/dual_b}"
RESULT_A_URL="rtmp://192.168.0.168/live/alertgateway"
SRS_API="${SRS_API:-http://192.168.0.168:1985/api/v1/streams/}"
SRS_HOST="${SRS_HOST:-192.168.0.168}"
SRS_RTMP_PORT="${SRS_RTMP_PORT:-1935}"
DUAL_WIDTH="${DUAL_WIDTH:-1920}"
DUAL_HEIGHT="${DUAL_HEIGHT:-1080}"
DUAL_FPS="${DUAL_FPS:-30}"
DUAL_BITRATE_KBPS="${DUAL_BITRATE_KBPS:-8000}"
RUN_ID="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="${LOG_DIR:-/tmp/alertgateway_dual_${RUN_ID}}"

CONFIG_CANDIDATE="$ROOT_DIR/config/config_4k_candidate_20260719.json"
CONFIG_4K="$ROOT_DIR/config/config_4k_18mbps.json"
CONFIG_A_LOCAL="$LOG_DIR/dual_a.json"
CONFIG_B_LOCAL="$LOG_DIR/dual_b.json"
REMOTE_CONFIG_A="/tmp/alertgateway_dual_${RUN_ID}_a.json"
REMOTE_CONFIG_B="/tmp/alertgateway_dual_${RUN_ID}_b.json"
REMOTE_RESULT_B="/tmp/alertgateway_dual_${RUN_ID}_b.flv"
REMOTE_LOG_A="/tmp/alertgateway_dual_${RUN_ID}_a.log"
REMOTE_LOG_B="/tmp/alertgateway_dual_${RUN_ID}_b.log"

PUSH_A_PID=""
PUSH_B_PID=""
BOARD_A_SSH_PID=""
BOARD_B_SSH_PID=""
BOARD_A_STARTED=0
BOARD_B_STARTED=0
SRS_WATCH_PID=""

mkdir -p "$LOG_DIR"

log() { printf '[%(%F %T)T] %s\n' -1 "$*"; }
fail() { log "ERROR: $*" >&2; exit 1; }

show_tail() {
  local title="$1" file="$2"
  [[ -s "$file" ]] || return 0
  log "$title（末尾 40 行）："
  tail -n 40 "$file" || true
}

fetch_srs() {
  local target="$1"
  curl --connect-timeout 3 --max-time 5 --fail --silent --show-error \
    "$SRS_API" >"$target"
}

stream_active() {
  local stream_name="$1" api_file="$2"
  python3 - "$api_file" "$stream_name" <<'PY'
import json
import sys

api_file, stream_name = sys.argv[1:]
with open(api_file, encoding="utf-8") as handle:
    payload = json.load(handle)
streams = payload if isinstance(payload, list) else payload.get("streams", [])
sys.exit(0 if any(
    item.get("name") == stream_name
    and (item.get("active") or item.get("publish", {}).get("active"))
    for item in streams
) else 1)
PY
}

wait_stream() {
  local stream_name="$1" timeout_sec="$2" api_file="$3"
  local deadline=$((SECONDS + timeout_sec))
  while (( SECONDS < deadline )); do
    if fetch_srs "$api_file" && stream_active "$stream_name" "$api_file"; then
      return 0
    fi
    # API 不可达时，直接从 RTMP 读取流元数据，避免把监控通道故障误判成流故障。
    if timeout 8 ffprobe -v error -rw_timeout 5000000 \
      -show_entries stream=codec_name,width,height -of default=noprint_wrappers=1 \
      "rtmp://192.168.0.168/live/${stream_name}" >"${api_file%.json}.probe.txt" 2>/dev/null; then
      return 0
    fi
    sleep 2
  done
  return 1
}

make_configs() {
  python3 - "$CONFIG_CANDIDATE" "$CONFIG_4K" "$CONFIG_A_LOCAL" "$CONFIG_B_LOCAL" \
    "$INPUT_A_URL" "$INPUT_B_URL" "$RESULT_A_URL" "$REMOTE_RESULT_B" \
    "$DUAL_WIDTH" "$DUAL_HEIGHT" "$DUAL_FPS" "$DUAL_BITRATE_KBPS" <<'PY'
import copy
import json
import sys

(candidate_path, base_path, output_a, output_b, input_a, input_b,
 result_a, result_b, width, height, fps, bitrate) = sys.argv[1:]

with open(candidate_path, encoding="utf-8") as handle:
    candidate = json.load(handle)
with open(base_path, encoding="utf-8") as handle:
    selected = json.load(handle)

def merge(base, override):
    for key, value in override.items():
        if isinstance(base.get(key), dict) and isinstance(value, dict):
            merge(base[key], value)
        else:
            base[key] = copy.deepcopy(value)

def make_config(input_url, result_url, sink, topic, client_id):
    config = copy.deepcopy(candidate)
    merge(config, selected)
    config.pop("active_config", None)
    config["source"] = {
        "type": "pull_stream",
        "url": input_url,
        "width": int(width),
        "height": int(height),
        "fps": int(fps),
        "reconnect_sec": 5,
    }
    config.setdefault("stream", {})["sink"] = sink
    if sink == "fixed_rtmp":
        config["stream"]["rtmp_url"] = result_url
        config["stream"].pop("path", None)
    else:
        config["stream"]["path"] = result_url
        config["stream"].pop("rtmp_url", None)
    config["stream"]["bitrate_kbps"] = int(bitrate)
    config.setdefault("mqtt", {})["topic"] = topic
    config["mqtt"]["client_id"] = client_id
    return config

with open(output_a, "w", encoding="utf-8") as handle:
    json.dump(make_config(input_a, result_a, "fixed_rtmp", "desk/dual/a", "AlertGateway-dual-a"),
              handle, ensure_ascii=False, indent=2)
    handle.write("\n")
with open(output_b, "w", encoding="utf-8") as handle:
    json.dump(make_config(input_b, result_b, "local_flv", "desk/dual/b", "AlertGateway-dual-b"),
              handle, ensure_ascii=False, indent=2)
    handle.write("\n")
PY
}

start_srs_watch() {
  local watch_log="$LOG_DIR/srs_watch.jsonl"
  local watch_tmp="$LOG_DIR/srs_watch_current.json"
  (
    while true; do
      if fetch_srs "$watch_tmp"; then
        printf '%(%FT%T%z)T\t' -1 >>"$watch_log"
        tr -d '\n' <"$watch_tmp" >>"$watch_log"
        printf '\n' >>"$watch_log"
      fi
      sleep 10
    done
  ) &
  SRS_WATCH_PID=$!
  log "已开始每 10 秒记录 SRS 输入/结果流快照：$watch_log"
}

stop_srs_watch() {
  if [[ -n "$SRS_WATCH_PID" ]] && kill -0 "$SRS_WATCH_PID" 2>/dev/null; then
    kill "$SRS_WATCH_PID" 2>/dev/null || true
    wait "$SRS_WATCH_PID" 2>/dev/null || true
  fi
  SRS_WATCH_PID=""
}

stop_local_process() {
  local pid="$1"
  if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
    kill -INT "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
}

stop_board_process() {
  local config_path="$1" ssh_pid="$2"
  ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
    "pkill -INT -f 'AlertGateway ${config_path}' || true" >/dev/null 2>&1 || true
  if [[ -n "$ssh_pid" ]] && kill -0 "$ssh_pid" 2>/dev/null; then
    wait "$ssh_pid" 2>/dev/null || true
  fi
}

collect_board_artifacts() {
  ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
    "test -f '$REMOTE_LOG_A' && cat '$REMOTE_LOG_A'" >"$LOG_DIR/board_a.log" 2>/dev/null || true
  ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
    "test -f '$REMOTE_LOG_B' && cat '$REMOTE_LOG_B'" >"$LOG_DIR/board_b.log" 2>/dev/null || true
  ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
    "if test -f '$REMOTE_RESULT_B'; then stat -c 'file=%n size_bytes=%s' '$REMOTE_RESULT_B'; fi; \
     if command -v ffprobe >/dev/null 2>&1 && test -f '$REMOTE_RESULT_B'; then \
       ffprobe -v error -show_entries format=duration,size \
         -show_entries stream=codec_name,width,height,nb_read_frames,r_frame_rate \
         -of default=noprint_wrappers=1 '$REMOTE_RESULT_B'; \
     fi" >"$LOG_DIR/dual_b_media.txt" 2>&1 || true
}

cleanup() {
  local code=$?
  trap - EXIT INT TERM
  stop_srs_watch
  stop_local_process "$PUSH_A_PID"
  stop_local_process "$PUSH_B_PID"
  if (( BOARD_A_STARTED )); then
    stop_board_process "$REMOTE_CONFIG_A" "$BOARD_A_SSH_PID"
    BOARD_A_STARTED=0
  fi
  if (( BOARD_B_STARTED )); then
    stop_board_process "$REMOTE_CONFIG_B" "$BOARD_B_SSH_PID"
    BOARD_B_STARTED=0
  fi
  collect_board_artifacts
  ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
    "rm -f '$REMOTE_CONFIG_A' '$REMOTE_CONFIG_B'" >/dev/null 2>&1 || true
  if (( code != 0 )); then
    show_tail "A 路发布端日志" "$LOG_DIR/publisher_a.log"
    show_tail "B 路发布端日志" "$LOG_DIR/publisher_b.log"
    show_tail "A 路板端日志" "$LOG_DIR/board_a.log"
    show_tail "B 路板端日志" "$LOG_DIR/board_b.log"
    log "B 路结果文件及媒体信息：$LOG_DIR/dual_b_media.txt"
    log "调试文件保留在：$LOG_DIR"
  fi
  exit "$code"
}
trap cleanup EXIT INT TERM

[[ "$RUN_SEC" =~ ^[1-9][0-9]*$ ]] || fail "运行秒数必须是正整数，当前值：$RUN_SEC"
[[ "$DUAL_WIDTH" =~ ^[1-9][0-9]*$ && "$DUAL_HEIGHT" =~ ^[1-9][0-9]*$ && "$DUAL_FPS" =~ ^[1-9][0-9]*$ ]] \
  || fail "双路分辨率和 FPS 必须是正整数"
[[ "$DUAL_BITRATE_KBPS" =~ ^[1-9][0-9]*$ ]] || fail "双路码率必须是正整数"
[[ -f "$VIDEO_A" ]] || fail "找不到 A 路视频：$VIDEO_A"
[[ -f "$VIDEO_B" ]] || fail "找不到 B 路视频：$VIDEO_B"
for command in ffmpeg ffprobe curl python3 ssh scp timeout; do
  command -v "$command" >/dev/null || fail "缺少命令：$command"
done

log "=== 双路候选并发 PoC ==="
log "A 路输入：$VIDEO_A -> $INPUT_A_URL -> $RESULT_A_URL"
log "B 路输入：$VIDEO_B -> $INPUT_B_URL -> 板端 $REMOTE_RESULT_B"
log "板端：$BOARD_HOST"
log "规格：${DUAL_WIDTH}x${DUAL_HEIGHT}@${DUAL_FPS}，每路目标 ${DUAL_BITRATE_KBPS} kbps，运行 ${RUN_SEC} 秒"
log "本地调试目录：$LOG_DIR"

ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,width,height,r_frame_rate \
  -of default=noprint_wrappers=1 "$VIDEO_A" >"$LOG_DIR/input_a.txt" \
  || fail "ffprobe 无法读取 A 路输入"
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,width,height,r_frame_rate \
  -of default=noprint_wrappers=1 "$VIDEO_B" >"$LOG_DIR/input_b.txt" \
  || fail "ffprobe 无法读取 B 路输入"

log "检查 SRS 前置条件；SRS 只允许手动启动，测试脚本不会启动或重启它。"
bash tools/test/check_srs_manual.sh "$SRS_API" "$SRS_HOST" "$SRS_RTMP_PORT" \
  "$LOG_DIR/srs_before.json" || fail "SRS 未就绪，未启动本次测试。"

log "检查板端二进制、候选模型和现有 AlertGateway 进程。"
ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "cd $BOARD_DIR && test -x AlertGateway && \
   test -f model/yolov8s_4k_photos_head_20e_20260718_finaltest_int8.rknn" \
  || fail "板端缺少 AlertGateway/候选 RKNN，或 SSH 不可达：$BOARD_HOST"
if ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "pgrep -af 'AlertGateway' | grep -v 'pgrep -af'"; then
  fail "板端已有 AlertGateway 在运行。本脚本不会自动中断已有业务。"
fi

make_configs
log "生成临时双路完整配置：$CONFIG_A_LOCAL、$CONFIG_B_LOCAL"
scp "$CONFIG_A_LOCAL" "$BOARD_HOST:$REMOTE_CONFIG_A" >/dev/null
scp "$CONFIG_B_LOCAL" "$BOARD_HOST:$REMOTE_CONFIG_B" >/dev/null
ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "test -s '$REMOTE_CONFIG_A' && test -s '$REMOTE_CONFIG_B'" \
  || fail "双路临时配置未能同步到板端"

log "启动两路 1080p 输入发布。"
timeout --preserve-status --signal=INT "$((RUN_SEC + 45))s" \
  ffmpeg -hide_banner -loglevel info -re -stream_loop -1 -i "$VIDEO_A" \
  -map 0:v:0 -vf "scale=${DUAL_WIDTH}:${DUAL_HEIGHT}:flags=fast_bilinear" \
  -r "$DUAL_FPS" -c:v libx264 -preset veryfast -tune zerolatency \
  -pix_fmt yuv420p -g "$((DUAL_FPS * 2))" -f flv "$INPUT_A_URL" \
  >"$LOG_DIR/publisher_a.log" 2>&1 &
PUSH_A_PID=$!
timeout --preserve-status --signal=INT "$((RUN_SEC + 45))s" \
  ffmpeg -hide_banner -loglevel info -re -stream_loop -1 -i "$VIDEO_B" \
  -map 0:v:0 -vf "scale=${DUAL_WIDTH}:${DUAL_HEIGHT}:flags=fast_bilinear" \
  -r "$DUAL_FPS" -c:v libx264 -preset veryfast -tune zerolatency \
  -pix_fmt yuv420p -g "$((DUAL_FPS * 2))" -f flv "$INPUT_B_URL" \
  >"$LOG_DIR/publisher_b.log" 2>&1 &
PUSH_B_PID=$!

if ! wait_stream "dual_a" 30 "$LOG_DIR/srs_input_a.json"; then
  fail "A 路输入 live/dual_a 在 30 秒内未 active"
fi
if ! wait_stream "dual_b" 30 "$LOG_DIR/srs_input_b.json"; then
  fail "B 路输入 live/dual_b 在 30 秒内未 active"
fi
log "两路输入均已 active，启动两个板端推理进程。"

ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "cd $BOARD_DIR && timeout --preserve-status --signal=INT --kill-after=5s \
   '${RUN_SEC}s' ./AlertGateway '$REMOTE_CONFIG_A' >'$REMOTE_LOG_A' 2>&1" &
BOARD_A_SSH_PID=$!
BOARD_A_STARTED=1
ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "cd $BOARD_DIR && timeout --preserve-status --signal=INT --kill-after=5s \
   '${RUN_SEC}s' ./AlertGateway '$REMOTE_CONFIG_B' >'$REMOTE_LOG_B' 2>&1" &
BOARD_B_SSH_PID=$!
BOARD_B_STARTED=1

if ! wait_stream "alertgateway" 35 "$LOG_DIR/srs_result_a.json"; then
  fail "A 路结果流 live/alertgateway 在 35 秒内未 active"
fi
log "A 路结果流已 active，可用播放器观看：$RESULT_A_URL"
start_srs_watch
log "B 路不创建第二个 RTMP 结果地址，结果保留在板端：$REMOTE_RESULT_B"
log "MQTT 数据分别使用 desk/dual/a 和 desk/dual/b。"

BOARD_B_OUTPUT_STARTED=0
for _ in {1..20}; do
  if ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
    "test -s '$REMOTE_RESULT_B'" >/dev/null 2>&1; then
    log "B 路已开始写入临时 FLV。"
    BOARD_B_OUTPUT_STARTED=1
    break
  fi
  sleep 2
done
(( BOARD_B_OUTPUT_STARTED )) || fail "B 路未开始写入临时 FLV，请查看 B 路板端日志"

set +e
wait "$BOARD_A_SSH_PID"
BOARD_A_RC=$?
wait "$BOARD_B_SSH_PID"
BOARD_B_RC=$?
set -e
BOARD_A_STARTED=0
BOARD_B_STARTED=0
if (( BOARD_A_RC != 0 || BOARD_B_RC != 0 )); then
  fail "双路板端进程异常退出：A=$BOARD_A_RC B=$BOARD_B_RC"
fi

log "=== 双路运行完成 ==="
log "日志：$LOG_DIR"
log "A 路结果：$RESULT_A_URL"
log "B 路结果文件：$REMOTE_RESULT_B（详见 $LOG_DIR/dual_b_media.txt）"
log "关键统计将在退出清理阶段收集到 board_a.log、board_b.log。"
