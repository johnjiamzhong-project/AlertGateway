#!/usr/bin/env bash
# Run one isolated 4K candidate playback: host publisher -> SRS -> RK3588 -> SRS result.
# It never changes config/config.json or kills a pre-existing AlertGateway process.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

VIDEO="${1:-runs/input_videos/4k/VID_20260712_131410.mp4}"
RUN_SEC="${2:-120}"
BOARD_HOST="${BOARD_HOST:-firefly@192.168.0.200}"
BOARD_DIR="${BOARD_DIR:-~/AlertGateway}"
CONFIG="${CONFIG:-config/config_4k_candidate_20260719.json}"
INPUT_URL="${INPUT_URL:-rtmp://192.168.0.168/live/testsrc2}"
RESULT_URL="${RESULT_URL:-rtmp://192.168.0.168/live/alertgateway}"
SRS_API="${SRS_API:-http://192.168.0.168:1985/api/v1/streams/}"
RUN_ID="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="${LOG_DIR:-/tmp/alertgateway_4k_${RUN_ID}}"
PUSH_LOG="$LOG_DIR/publisher.log"
BOARD_LOG="/tmp/alertgateway_4k_${RUN_ID}.log"
BOARD_LOG_COPY="$LOG_DIR/board.log"
API_BEFORE="$LOG_DIR/srs_before.json"
API_INPUT="$LOG_DIR/srs_input.json"
API_OUTPUT="$LOG_DIR/srs_output.json"
SRS_WATCH_LOG="$LOG_DIR/srs_watch.jsonl"
SRS_WATCH_TMP="$LOG_DIR/srs_watch_current.json"
PUSH_PID=""
BOARD_SSH_PID=""
SRS_WATCH_PID=""
BOARD_STARTED=0

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
  curl --connect-timeout 3 --max-time 5 --fail --silent --show-error "$SRS_API" >"$target"
}
stream_active() {
  local stream_name="$1" api_file="$2"
  grep -Eq "\"name\":\"${stream_name}\".*\"active\":true" "$api_file"
}
wait_stream() {
  local stream_name="$1" timeout_sec="$2" api_file="$3"
  local deadline=$((SECONDS + timeout_sec))
  while (( SECONDS < deadline )); do
    if fetch_srs "$api_file" && stream_active "$stream_name" "$api_file"; then
      return 0
    fi
    sleep 2
  done
  return 1
}
start_srs_watch() {
  # SRS 的 streams API 会给出 clients、recv_30s、send_30s。保留完整快照而非
  # 在脚本中猜测 JSON 字段位置，便于在播放器观看 A/B 时复核真实接收吞吐。
  (
    while true; do
      if fetch_srs "$SRS_WATCH_TMP"; then
        printf '%(%FT%T%z)T\t' -1 >>"$SRS_WATCH_LOG"
        tr -d '\n' <"$SRS_WATCH_TMP" >>"$SRS_WATCH_LOG"
        printf '\n' >>"$SRS_WATCH_LOG"
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
cleanup() {
  local code=$?
  trap - EXIT INT TERM
  stop_srs_watch
  if [[ -n "$PUSH_PID" ]] && kill -0 "$PUSH_PID" 2>/dev/null; then
    log "停止本次输入发布进程 pid=$PUSH_PID"
    kill -INT "$PUSH_PID" 2>/dev/null || true
    wait "$PUSH_PID" 2>/dev/null || true
  fi
  if (( BOARD_STARTED )) && [[ -n "$BOARD_SSH_PID" ]] && kill -0 "$BOARD_SSH_PID" 2>/dev/null; then
    log "请求板端候选进程优雅退出"
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
      "pkill -INT -f './AlertGateway ${CONFIG}' || true" >/dev/null 2>&1 || true
    wait "$BOARD_SSH_PID" 2>/dev/null || true
  fi
  ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
    "test -f '$BOARD_LOG' && cat '$BOARD_LOG'" >"$BOARD_LOG_COPY" 2>/dev/null || true
  if (( code != 0 )); then
    show_tail "发布端日志" "$PUSH_LOG"
    show_tail "板端日志" "$BOARD_LOG_COPY"
    log "调试文件保留在：$LOG_DIR"
  fi
  exit "$code"
}
trap cleanup EXIT

[[ "$RUN_SEC" =~ ^[1-9][0-9]*$ ]] || fail "运行秒数必须是正整数，当前值：$RUN_SEC"
[[ -f "$VIDEO" ]] || fail "找不到输入视频：$VIDEO"
for command in ffmpeg ffprobe curl ssh; do command -v "$command" >/dev/null || fail "缺少命令：$command"; done

log "=== 4K 候选一键验证 ==="
log "视频：$VIDEO"
log "板端：$BOARD_HOST"
log "板端配置：$CONFIG"
log "输入：$INPUT_URL"
log "结果：$RESULT_URL"
log "本地调试目录：$LOG_DIR"
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,width,height,r_frame_rate -of default=noprint_wrappers=1 "$VIDEO" \
  || fail "ffprobe 无法读取输入视频"

if ! fetch_srs "$API_BEFORE"; then
  fail "SRS API 不可访问：$SRS_API。请先检查 Windows SRS 是否已启动且 1935/1985 正在监听。"
fi
log "SRS API 可访问。"

log "检查板端二进制、候选配置、基础配置和候选 RKNN。"
ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "cd $BOARD_DIR && test -x AlertGateway && test -f '$CONFIG' && test -f config/config_4k_8mbps.json && test -f model/yolov8s_4k_photos_head_20e_20260718_finaltest_int8.rknn" \
  || fail "板端缺少候选运行文件，或 SSH 不可达：$BOARD_HOST"
if ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "pgrep -af 'AlertGateway' | grep -v 'pgrep -af'"; then
  fail "板端已有 AlertGateway 在运行。为避免中断其他测试，本脚本不会自动杀进程。"
fi

log "启动输入发布，等待 SRS 出现 live/testsrc2。"
timeout --preserve-status --signal=INT "$((RUN_SEC + 30))s" \
  bash tools/test/push_testsrc2_4k_to_srs.sh "$VIDEO" "$INPUT_URL" 0 >"$PUSH_LOG" 2>&1 &
PUSH_PID=$!
if ! wait_stream "testsrc2" 20 "$API_INPUT"; then
  fail "输入流 live/testsrc2 在 20 秒内未变为 active；优先查看发布端日志、SRS RTMP 服务和视频编码。"
fi
log "输入流已 active，启动 RK3588 候选推理。"
ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "cd $BOARD_DIR && timeout --preserve-status --signal=INT --kill-after=5s '${RUN_SEC}s' ./AlertGateway '$CONFIG' >'$BOARD_LOG' 2>&1" &
BOARD_SSH_PID=$!
BOARD_STARTED=1

if ! wait_stream "alertgateway" 35 "$API_OUTPUT"; then
  fail "结果流 live/alertgateway 在 35 秒内未变为 active；请检查板端日志中的 PullStream、RKNN、MPP 或 RTMP 错误。"
fi
log "结果流已 active，可在播放器拉取：$RESULT_URL"
start_srs_watch
log "已开始每 10 秒记录 SRS 客户端与吞吐快照：$SRS_WATCH_LOG"
log "运行 ${RUN_SEC} 秒；按 Ctrl+C 可提前结束，脚本会仅清理本次启动的进程。"
wait "$BOARD_SSH_PID" || fail "板端候选进程异常退出"
BOARD_STARTED=0
stop_srs_watch
if [[ -n "$PUSH_PID" ]] && kill -0 "$PUSH_PID" 2>/dev/null; then
  log "板端运行结束，停止本次输入发布进程 pid=$PUSH_PID"
  kill -INT "$PUSH_PID" 2>/dev/null || true
fi
wait "$PUSH_PID" 2>/dev/null || true
PUSH_PID=""

fetch_srs "$LOG_DIR/srs_final.json" || true
ssh -o BatchMode=yes -o ConnectTimeout=5 "$BOARD_HOST" \
  "test -f '$BOARD_LOG' && cat '$BOARD_LOG'" >"$BOARD_LOG_COPY" || true
log "=== 运行完成 ==="
log "日志：$PUSH_LOG"
log "板端日志副本：$BOARD_LOG_COPY"
log "SRS 观看快照：$SRS_WATCH_LOG"
log "关键板端统计："
grep -E '\[Infer\]|EncodeStats|StreamStats|PullStats|ERROR|Error|failed|Failed' "$BOARD_LOG_COPY" | tail -n 30 || true
