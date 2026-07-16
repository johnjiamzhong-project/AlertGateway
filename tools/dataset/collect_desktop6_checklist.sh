#!/usr/bin/env bash
set -euo pipefail

# ANSI color codes
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${BLUE}========================================================================${NC}"
echo -e "${GREEN}             AlertGateway 桌面六类数据采集与整理 Checklist${NC}"
echo -e "${BLUE}========================================================================${NC}"
echo ""
echo -e "${YELLOW}[重要提示]${NC} 采集和标注训练数据默认${RED}不需要${NC}开启 SRS 服务！"
echo -e "           只有进行全链路 RTMP 推流 Smoke 验证或远程拉流抓图时，才需要开启 SRS。"
echo ""

echo -e "${GREEN}第 1 步：板端环境确认${NC}"
echo -e "  请确保登录板端并验证以下关键文件是否存在："
echo -e "  - ${BLUE}~/AlertGateway/AlertGateway${NC} (生产可执行二进制)"
echo -e "  - ${BLUE}~/AlertGateway/config/config.json${NC} (生产配置文件)"
echo -e "  - ${BLUE}~/AlertGateway/model/yolov8s_rockchip_dfl.rknn${NC} (官方九输出量化模型)"
echo ""

echo -e "${GREEN}第 2 步：临时配置修改（板端）${NC}"
echo -e "  编辑板端 ${BLUE}~/AlertGateway/config/config.json${NC}，临时修改 stream 字段："
echo -e "  - 将 ${YELLOW}stream.rtmp_url${NC} 临时设置为本地路径: ${BLUE}/tmp/collect_desktop.flv${NC}"
echo -e "  - 设置 ${YELLOW}stream.bitrate_kbps${NC} 建议为: ${BLUE}2000${NC}"
echo -e "  - 设置 ${YELLOW}stream.draw_detection_labels${NC} 为: ${BLUE}true${NC}"
echo ""

echo -e "${GREEN}第 3 步：运行板端采集并保存视频${NC}"
echo -e "  在板端终端中执行以下命令开始采集（限制运行时间 60 秒）："
echo -e "  ${YELLOW}cd ~/AlertGateway${NC}"
echo -e "  ${YELLOW}timeout 60 ./AlertGateway config/config.json${NC}"
echo ""

echo -e "${GREEN}第 4 步：恢复板端配置（板端）${NC}"
echo -e "  采集完成后，必须将 ${BLUE}~/AlertGateway/config/config.json${NC} 恢复为原生产 RTMP 服务器地址！"
echo -e "  ${RED}注意：${NC} 避免后续真机 smoke 误写本地文件造成存储溢出或流异常。"
echo ""

echo -e "${GREEN}第 5 步：拉回视频并创建抽帧目录（Host 主机）${NC}"
echo -e "  在 Host 主机上运行以下命令，将采集到的视频传输到主机，并创建临时解压目录："
echo -e "  ${YELLOW}scp firefly@192.168.0.200:/tmp/collect_desktop.flv /home/rambos/datasets/${NC}"
echo -e "  ${YELLOW}mkdir -p /home/rambos/datasets/raw_extracted${NC}"
echo ""

echo -e "${GREEN}第 6 步：执行视频等间隔抽帧（Host 主机）${NC}"
echo -e "  在 Host 主机上使用 ffmpeg 进行 1 FPS 抽帧，过滤冗余帧并降低画面相似度："
echo -e "  ${YELLOW}ffmpeg -i /home/rambos/datasets/collect_desktop.flv -vf \"fps=1\" -q:v 2 /home/rambos/datasets/raw_extracted/raw_desktop_%04d.jpg${NC}"
echo ""

echo -e "${GREEN}第 7 步：后续数据处理建议（Host 主机）${NC}"
echo -e "  - ${YELLOW}人工筛选与去重${NC}：剔除模糊、严重失焦及高度重合的图片。"
echo -e "  - ${YELLOW}标注与分割${NC}：使用标注工具进行目标框标注（标签类别对齐为规定的 6 类）。"
echo -e "  - ${YELLOW}数据集划分${NC}：将数据集按 8:1:1 比例归入 train/val/test 的 images 和 labels 子目录下。"
echo -e "  - ${RED}时序隔离防泄漏：${NC} 不要将同一次录像的多张连续抽帧图片同时交叉分在 train 和 val 集中。"
echo ""
echo -e "${BLUE}========================================================================${NC}"
