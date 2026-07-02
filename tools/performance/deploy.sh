#!/bin/bash
# deploy.sh - 一键将本地的控制脚本和服务部署到板端并启用

BOARD_USER="firefly"
BOARD_IP="192.168.0.200"
BOARD_DIR="/home/firefly/AlertGateway"

# 获取当前脚本所在目录
DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

echo "==== 1. 创建板端临时目录并推送文件 ===="
ssh ${BOARD_USER}@${BOARD_IP} "mkdir -p ${BOARD_DIR}/tools"
scp "${DIR}/rockchip_performance.sh" "${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/tools/"
scp "${DIR}/rockchip-performance.service" "${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/tools/"

echo "==== 2. 板端安装并注册服务 ===="
ssh -t ${BOARD_USER}@${BOARD_IP} "
    sudo cp ${BOARD_DIR}/tools/rockchip_performance.sh /usr/local/bin/ && \
    sudo chmod +x /usr/local/bin/rockchip_performance.sh && \
    sudo cp ${BOARD_DIR}/tools/rockchip-performance.service /etc/systemd/system/ && \
    sudo systemctl daemon-reload && \
    sudo systemctl enable --now rockchip-performance && \
    echo '==== 3. 服务当前状态 ====' && \
    sudo systemctl status rockchip-performance && \
    echo '==== 4. 当前硬件调频状态 ====' && \
    sudo /usr/local/bin/rockchip_performance.sh status
"
