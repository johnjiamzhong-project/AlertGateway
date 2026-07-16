#!/usr/bin/env bash
set -euo pipefail

# Ensure output directory exists
OUT_DIR="runs/testsrc2/quality_compare_12v18_20260714"
mkdir -p "$OUT_DIR"

cd "$(dirname "$0")/../.."

echo "Cleaning up conflicting background processes..."
# Kill any background ffmpeg publishers on the host
killall -9 ffmpeg || true
# Kill any background AlertGateway processes on the board
ssh -o BatchMode=yes firefly@192.168.0.200 "killall -9 AlertGateway || killall -9 AlertGateway.maxfps_20260714 || true"

echo "Starting 12 Mbps vs 18 Mbps 4K Comparison Runs..."

for spec in "12000 config_testsrc2_4k_12m.json 12m" "18000 config_testsrc2_4k_18m.json 18m"; do
    read -r rate cfg tag <<< "$spec"
    echo "=========================================="
    echo "Running comparison for $tag ($rate kbps)..."
    echo "=========================================="

    tmp_output="/tmp/output_${tag}.flv"
    final_output="$OUT_DIR/output_${tag}.flv"
    push_log="/tmp/ag_push_${tag}.log"
    board_log="$OUT_DIR/board_${tag}.log"
    capture_log="/tmp/ag_capture_${tag}.log"

    rm -f "$tmp_output" "$final_output" "$push_log" "$board_log" "$capture_log"

    echo "1. Starting RTMP stream publisher (input: VID_20260712_131410.mp4 from beginning)..."
    timeout 50s bash tools/test/push_testsrc2_4k_to_srs.sh \
        runs/input_videos/4k/VID_20260712_131410.mp4 \
        rtmp://192.168.0.168/live/testsrc2 42 >"$push_log" 2>&1 &
    push_pid=$!

    # Wait a bit for the publisher to connect and establish the stream
    sleep 4

    echo "2. Starting AlertGateway binary on RK3588 board..."
    ssh -o BatchMode=yes firefly@192.168.0.200 \
        "cd ~/AlertGateway && timeout --signal=INT --kill-after=4s 36s \
        ./AlertGateway.maxfps_20260714 config/${cfg} \
        >/tmp/ag_board_${tag}.log 2>&1" &
    board_pid=$!

    # Wait for the board pipeline to spin up and stabilize
    echo "3. Waiting 12s for the pipeline to stabilize..."
    sleep 12

    echo "4. Capturing H.264 stream output from SRS (rtmp://192.168.0.168/live/testsrc2_result)..."
    ffmpeg -y -hide_banner -loglevel error \
        -i rtmp://192.168.0.168/live/testsrc2_result -t 20 \
        -c copy "$tmp_output" >"$capture_log" 2>&1 || true

    echo "5. Cleaning up processes..."
    kill "$push_pid" 2>/dev/null || true
    wait "$push_pid" 2>/dev/null || true
    # Ask board to exit cleanly
    ssh -o BatchMode=yes firefly@192.168.0.200 "killall -INT AlertGateway || true"
    wait "$board_pid" 2>/dev/null || true

    echo "6. Retrieving logs from board..."
    ssh -o BatchMode=yes firefly@192.168.0.200 \
        "cat /tmp/ag_board_${tag}.log" > "$board_log" || true

    if [[ -f "$tmp_output" && -s "$tmp_output" ]]; then
        mv "$tmp_output" "$final_output"
    else
        echo "ERROR: Output file was not created or is empty!"
        cat "$capture_log" || true
        exit 1
    fi

    echo "Run complete for $tag. Stream stats:"
    ffprobe -v error -show_entries format=duration,size,bit_rate \
        -show_entries stream=width,height,r_frame_rate,avg_frame_rate \
        -of default=noprint_wrappers=1 "$final_output" || true

    echo "Last logs from board for $tag:"
    grep -E 'EncodeStats|StreamStats|PullStats' "$board_log" | tail -4 || true
    echo ""

    # Sleep to allow SRS server to fully close the RTMP mount point and avoid collision
    echo "Waiting 6s for SRS to release stream..."
    sleep 6
done

echo "All runs completed successfully."
