#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

for spec in "6000 config_testsrc2_4k_6m.json" "8000 config_testsrc2_4k_8m.json"; do
    read -r rate cfg <<< "$spec"
    tag="${rate}k_sync"
    output="/tmp/ag_4k_sync_${tag}.flv"
    push_log="/tmp/ag_4k_sync_push_${tag}.log"
    board_log="runs/testsrc2/board_4k_sync_${tag}_20260713.log"
    rm -f "$output" "$push_log" "$board_log"

    timeout 42s bash tools/test/push_testsrc2_4k_to_srs.sh \
        runs/input_videos/4k/VID_20260712_131410.mp4 \
        rtmp://192.168.0.168/live/testsrc2 36 >"$push_log" 2>&1 &
    push_pid=$!

    ssh -o BatchMode=yes firefly@192.168.0.200 \
        "cd ~/AlertGateway && timeout --signal=INT --kill-after=4s 28s \
        ./AlertGateway.instrumented_20260713 config/${cfg} \
        >/tmp/ag_4k_sync_${tag}_board.log 2>&1" &
    board_pid=$!

    sleep 8
    timeout 22s ffmpeg -hide_banner -loglevel error -rw_timeout 5000000 \
        -i rtmp://192.168.0.168/live/testsrc2_result -t 16 \
        -map 0:v:0 -c copy "$output" >/tmp/ag_4k_sync_capture_${tag}.log 2>&1 || true

    kill "$push_pid" 2>/dev/null || true
    wait "$push_pid" 2>/dev/null || true
    wait "$board_pid" 2>/dev/null || true
    ssh -o BatchMode=yes firefly@192.168.0.200 \
        "cat /tmp/ag_4k_sync_${tag}_board.log" > "$board_log" || true

    echo "RATE=${rate}"
    ffprobe -v error -show_entries format=duration,size,bit_rate \
        -show_entries stream=width,height,r_frame_rate,avg_frame_rate \
        -of default=noprint_wrappers=1 "$output" || true
    grep -E 'EncodeStats|StreamStats|PullStats' "$board_log" | tail -4 || true
done
