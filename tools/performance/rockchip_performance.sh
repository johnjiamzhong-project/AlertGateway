#!/bin/bash
# rockchip_performance.sh - Manage CPU and NPU performance mode for RK3588

CPU_GOVERNORS="/sys/devices/system/cpu/cpufreq/policy*/scaling_governor"
NPU_GOVERNORS="/sys/class/devfreq/*npu*/governor"

BACKUP_CPU_FILE="/var/run/rockchip_cpu_governor.bak"
BACKUP_NPU_FILE="/var/run/rockchip_npu_governor.bak"

if [ "$EUID" -ne 0 ]; then
    echo "Error: Please run as root." >&2
    exit 1
fi

enable_performance() {
    echo "Enabling performance mode..."
    
    # Backup CPU governors
    local cpu_states=""
    for gov in $CPU_GOVERNORS; do
        if [ -f "$gov" ]; then
            cpu_states="$cpu_states $gov:$(cat $gov)"
        fi
    done
    echo "$cpu_states" > "$BACKUP_CPU_FILE" 2>/dev/null
    
    # Set CPU to performance
    for gov in $CPU_GOVERNORS; do
        if [ -f "$gov" ]; then
            echo performance > "$gov"
        fi
    done

    # Backup NPU governors
    local npu_states=""
    for gov in $NPU_GOVERNORS; do
        if [ -f "$gov" ]; then
            npu_states="$npu_states $gov:$(cat $gov)"
        fi
    done
    echo "$npu_states" > "$BACKUP_NPU_FILE" 2>/dev/null
    
    # Set NPU to performance
    for gov in $NPU_GOVERNORS; do
        if [ -f "$gov" ]; then
            echo performance > "$gov"
        fi
    done
    
    echo "Performance mode enabled."
}

disable_performance() {
    echo "Disabling performance mode (restoring previous governors)..."
    
    # Restore CPU
    if [ -f "$BACKUP_CPU_FILE" ]; then
        for item in $(cat "$BACKUP_CPU_FILE"); do
            local gov_path="${item%%:*}"
            local gov_val="${item#*:}"
            if [ -f "$gov_path" ]; then
                echo "$gov_val" > "$gov_path"
            fi
        done
        rm -f "$BACKUP_CPU_FILE"
    else
        # Fallback to interactive
        for gov in $CPU_GOVERNORS; do
            if [ -f "$gov" ]; then
                echo interactive > "$gov" 2>/dev/null || echo schedutil > "$gov"
            fi
        done
    fi

    # Restore NPU
    if [ -f "$BACKUP_NPU_FILE" ]; then
        for item in $(cat "$BACKUP_NPU_FILE"); do
            local gov_path="${item%%:*}"
            local gov_val="${item#*:}"
            if [ -f "$gov_path" ]; then
                echo "$gov_val" > "$gov_path"
            fi
        done
        rm -f "$BACKUP_NPU_FILE"
    else
        # Fallback to rknpu_ondemand
        for gov in $NPU_GOVERNORS; do
            if [ -f "$gov" ]; then
                echo rknpu_ondemand > "$gov"
            fi
        done
    fi
    
    echo "Performance mode disabled."
}

show_status() {
    echo "=== CPU Governors ==="
    for gov in $CPU_GOVERNORS; do
        if [ -f "$gov" ]; then
            local policy=$(basename $(dirname "$gov"))
            echo "$policy: $(cat $gov)"
        fi
    done
    
    echo "=== NPU Governors ==="
    for gov in $NPU_GOVERNORS; do
        if [ -f "$gov" ]; then
            local dev=$(basename $(dirname "$gov"))
            echo "$dev: $(cat $gov)"
        fi
    done

    echo "=== Current Frequencies ==="
    for freq in /sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq; do
        if [ -f "$freq" ]; then
            local policy=$(basename $(dirname "$freq"))
            local mhz=$(($(cat $freq) / 1000))
            echo "$policy: ${mhz} MHz"
        fi
    done
    for freq in /sys/class/devfreq/*npu*/cur_freq; do
        if [ -f "$freq" ]; then
            local dev=$(basename $(dirname "$freq"))
            local mhz=$(($(cat $freq) / 1000000))
            echo "$dev: ${mhz} MHz"
        fi
    done
}

case "$1" in
    enable)
        enable_performance
        ;;
    disable)
        disable_performance
        ;;
    status)
        show_status
        ;;
    *)
        echo "Usage: $0 {enable|disable|status}"
        exit 1
        ;;
esac
