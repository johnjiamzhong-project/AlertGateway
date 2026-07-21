#!/usr/bin/env bash
# Check that the user has manually started the existing SRS instance.
# This script deliberately never starts, restarts, or stops SRS.
set -euo pipefail

SRS_API="${1:?usage: check_srs_manual.sh SRS_API SRS_HOST RTMP_PORT API_OUTPUT}"
SRS_HOST="${2:?usage: check_srs_manual.sh SRS_API SRS_HOST RTMP_PORT API_OUTPUT}"
SRS_RTMP_PORT="${3:?usage: check_srs_manual.sh SRS_API SRS_HOST RTMP_PORT API_OUTPUT}"
API_OUTPUT="${4:?usage: check_srs_manual.sh SRS_API SRS_HOST RTMP_PORT API_OUTPUT}"

for SRS_PREFLIGHT_COMMAND in curl python3 timeout; do
  command -v "$SRS_PREFLIGHT_COMMAND" >/dev/null || {
    printf '[SRS前置检查] 缺少命令：%s\n' "$SRS_PREFLIGHT_COMMAND" >&2
    exit 2
  }
done

SRS_PREFLIGHT_TMP="$(mktemp "${TMPDIR:-/tmp}/alertgateway_srs_api.XXXXXX")"
trap 'rm -f "$SRS_PREFLIGHT_TMP"' EXIT

api_ready() {
  curl --connect-timeout 3 --max-time 5 --fail --silent --show-error \
    "$SRS_API" >"$SRS_PREFLIGHT_TMP" 2>/dev/null || return 1
  python3 - "$SRS_PREFLIGHT_TMP" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as handle:
        payload = json.load(handle)
except (OSError, json.JSONDecodeError):
    raise SystemExit(1)

# SRS 5 exposes either a top-level list or {"streams": [...]}.  Requiring
# this shape avoids treating an unrelated HTTP service on port 1985 as SRS.
if isinstance(payload, list):
    raise SystemExit(0)
if isinstance(payload, dict) and isinstance(payload.get("streams"), list):
    raise SystemExit(0)
raise SystemExit(1)
PY
}

rtmp_port_ready() {
  timeout 3 bash -c ':</dev/tcp/$0/$1' "$SRS_HOST" "$SRS_RTMP_PORT" \
    >/dev/null 2>&1
}

while true; do
  API_OK=0
  RTMP_OK=0
  if api_ready; then API_OK=1; fi
  if rtmp_port_ready; then RTMP_OK=1; fi

  if (( API_OK && RTMP_OK )); then
    cp "$SRS_PREFLIGHT_TMP" "$API_OUTPUT"
    printf '[SRS前置检查] 已确认手动启动的 SRS：API=%s，RTMP=%s:%s\n' \
      "$SRS_API" "$SRS_HOST" "$SRS_RTMP_PORT"
    exit 0
  fi

  printf '\n[SRS前置检查] SRS 尚未就绪：'
  (( API_OK )) && printf 'API正常，' || printf 'API不可访问，'
  (( RTMP_OK )) && printf 'RTMP正常。\n' || printf 'RTMP端口不可访问。\n'
  printf '%s\n' \
    '请在 Windows 上手动启动唯一的 SRS 实例（使用 console.conf）；测试脚本不会启动、重启或停止 SRS。'
  printf '%s' 'SRS 启动完成后按 Enter 重新检测，输入 q 退出：'
  IFS= read -r SRS_PREFLIGHT_REPLY || {
    printf '\n[SRS前置检查] 输入已关闭，测试终止。\n' >&2
    exit 1
  }
  case "$SRS_PREFLIGHT_REPLY" in
    q|Q)
      printf '[SRS前置检查] 用户取消，测试终止。\n' >&2
      exit 1
      ;;
  esac
done
