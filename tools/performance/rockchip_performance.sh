#!/bin/bash
# rockchip_performance.sh - Manage CPU, NPU, DMC and CPUIdle performance mode for RK3588

CPU_GOVERNORS="/sys/devices/system/cpu/cpufreq/policy*/scaling_governor"
NPU_GOVERNORS="/sys/class/devfreq/*npu*/governor"
DMC_GOVERNORS="/sys/class/devfreq/dmc/governor"
CPUIDLE_STATES="/sys/devices/system/cpu/cpu*/cpuidle/state1/disable"

BACKUP_CPU_FILE="/var/run/rockchip_cpu_governor.bak"
BACKUP_NPU_FILE="/var/run/rockchip_npu_governor.bak"
BACKUP_DMC_FILE="/var/run/rockchip_dmc_governor.bak"
BACKUP_CPUIDLE_FILE="/var/run/rockchip_cpuidle.bak"

if [ "$EUID" -ne 0 ]; then
    echo "Error: Please run as root." >&2
    exit 1
fi

enable_performance() {
    echo "Enabling performance mode..."
    
    # 1. Backup & Set CPU to performance
    local cpu_states=""
    for gov in $CPU_GOVERNORS; do
        if [ -f "$gov" ]; then
            cpu_states="$cpu_states $gov:$(cat $gov)"
            echo performance > "$gov"
        fi
    done
    echo "$cpu_states" > "$BACKUP_CPU_FILE" 2>/dev/null

    # 2. Backup & Set NPU to performance
    local npu_states=""
    for gov in $NPU_GOVERNORS; do
        if [ -f "$gov" ]; then
            npu_states="$npu_states $gov:$(cat $gov)"
            echo performance > "$gov"
        fi
    done
    echo "$npu_states" > "$BACKUP_NPU_FILE" 2>/dev/null
    
    # 3. Backup & Set DMC (DDR) to performance (自动对齐最高频，避免硬编码)
    local dmc_states=""
    for gov in $DMC_GOVERNORS; do
        if [ -f "$gov" ]; then
            dmc_states="$dmc_states $gov:$(cat $gov)"
            echo performance > "$gov"
        fi
    done
    echo "$dmc_states" > "$BACKUP_DMC_FILE" 2>/dev/null

    # 4. Backup & Disable CPUIdle state1 (消除 NPU 中断唤醒延迟)
    local cpuidle_states=""
    for state in $CPUIDLE_STATES; do
        if [ -f "$state" ]; then
            cpuidle_states="$cpuidle_states $state:$(cat $state)"
            echo 1 > "$state"
        fi
    done
    echo "$cpuidle_states" > "$BACKUP_CPUIDLE_FILE" 2>/dev/null

    echo "Performance mode enabled."
}

disable_performance() {
    echo "Disabling performance mode (restoring previous governors)..."
    
    # 1. Restore CPU
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
        for gov in $CPU_GOVERNORS; do
            if [ -f "$gov" ]; then
                echo interactive > "$gov" 2>/dev/null || echo schedutil > "$gov"
            fi
        done
    fi

    # 2. Restore NPU
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
        for gov in $NPU_GOVERNORS; do
            if [ -f "$gov" ]; then
                echo rknpu_ondemand > "$gov"
            fi
        done
    fi

    # 3. Restore DMC (DDR)
    if [ -f "$BACKUP_DMC_FILE" ]; then
        for item in $(cat "$BACKUP_DMC_FILE"); do
            local gov_path="${item%%:*}"
            local gov_val="${item#*:}"
            if [ -f "$gov_path" ]; then
                echo "$gov_val" > "$gov_path"
            fi
        done
        rm -f "$BACKUP_DMC_FILE"
    else
        for gov in $DMC_GOVERNORS; do
            if [ -f "$gov" ]; then
                echo simple_ondemand > "$gov" 2>/dev/null || echo dmc_ondemand > "$gov"
            fi
        done
    fi

    # 4. Restore CPUIdle
    if [ -f "$BACKUP_CPUIDLE_FILE" ]; then
        for item in $(cat "$BACKUP_CPUIDLE_FILE"); do
            local state_path="${item%%:*}"
            local state_val="${item#*:}"
            if [ -f "$state_path" ]; then
                echo "$state_val" > "$state_path"
            fi
        done
        rm -f "$BACKUP_CPUIDLE_FILE"
    else
        for state in $CPUIDLE_STATES; do
            if [ -f "$state" ]; then
                echo 0 > "$state"
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

    echo "=== DMC (DDR) Governors ==="
    for gov in $DMC_GOVERNORS; do
        if [ -f "$gov" ]; then
            echo "dmc: $(cat $gov)"
        fi
    done

    echo "=== CPUIdle State1 Disable ==="
    if [ -f /sys/devices/system/cpu/cpu0/cpuidle/state1/disable ]; then
        echo "cpu0 state1/disable: $(cat /sys/devices/system/cpu/cpu0/cpuidle/state1/disable)"
    fi

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
    for freq in /sys/class/devfreq/dmc/cur_freq; do
        if [ -f "$freq" ]; then
            local mhz=$(($(cat $freq) / 1000000))
            echo "dmc: ${mhz} MHz"
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
