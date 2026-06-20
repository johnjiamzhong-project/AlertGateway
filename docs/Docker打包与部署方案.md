# Docker 打包与部署方案

本文档规划如何用 Docker 改造 AlertGateway 当前的"WSL2 手动配置交叉编译环境 + scp 裸机部署"流程。WSL 端和板子端的 Docker 用途不同，分开讨论。

---

## 现状回顾

- **WSL2（构建端）**：手动安装 `aarch64-linux-gnu-g++ 11.4.0`，手动维护 `/home/rambos/sysroot`（板子厂商 FFmpeg-rockchip 6.1 头文件等），`third_party/*.so` 需要从板子上手动 scp 下来对齐 ABI（见 `BUGS.md`、各 ABI 踩坑记录）
- **RK3588S（运行端）**：`AlertGateway` 二进制 + `config/config.json` 通过 `scp` 上传到板子 `~/AlertGateway/`，用 `start.sh` 以裸机进程方式启停（`nohup` + PID 文件）
- 依赖的板载库（`librknnrt.so`、`librockchip_mpp.so`、`libpaho-mqtt3c`）都是**预装在板子系统镜像里**的厂商私有二进制，不随仓库分发

---

## 方案一：WSL 端 — 交叉编译构建容器

### 目标
把"工具链版本 + sysroot 路径 + 编译命令"固化进 Dockerfile，新机器/新人不需要再手动装 `aarch64-linux-gnu-g++`、手动建 `/home/rambos/sysroot`。

### 镜像内容（`docker/Dockerfile.build`）
```
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
    cmake make git
WORKDIR /workspace
```

只装工具链，**不**把 sysroot 和 `third_party/*.so` 打进镜像：
- `sysroot`（316M，含厂商 FFmpeg-rockchip 头文件）路径/内容因人而异，且不应固化进可分发镜像
- `third_party/*.so`（paho/rga/rknn stub）是从板子上 scp 下来的版本相关二进制，`.gitignore` 已明确标注"无重分发授权"

这两块在**运行容器时**用 `-v` 挂载，镜像本身保持精简、可分享。

### 构建与运行

```bash
# 构建镜像（只需一次，或工具链版本变化时）
docker build -t alertgateway-builder -f docker/Dockerfile.build .

# 编译（仓库 + sysroot 都挂载进去，CMakeLists.txt 里硬编码的
# /home/rambos/sysroot 路径不用改，挂载到容器内同名路径即可）
docker run --rm \
  -v "$(pwd)":/workspace \
  -v /home/rambos/sysroot:/home/rambos/sysroot:ro \
  -w /workspace \
  alertgateway-builder \
  bash -c "cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake && cmake --build build -j\$(nproc)"
```

产物 `build/AlertGateway` 直接出现在宿主机仓库目录里（bind mount），后续 `scp` 到板子的步骤不变。

### 需要确认 / 注意事项
- `third_party/*.so` 仍需按 `readme.md` 现有流程从板子手动 scp 下来放好，Docker 化不改变这一步，只是把"编译"这一步容器化
- 如果想完全去掉对 `/home/rambos/sysroot` 绝对路径的依赖，可以把 `CMakeLists.txt` 里的 `SYSROOT` 改成可通过 `-DSYSROOT=...` 传入的 cache 变量（目前是硬编码，见 `CMakeLists.txt:18`）—— 这是可选的后续优化，不是本方案的必需项

---

## 方案二：板子端部署

### 推荐：维持裸机部署，不引入 Docker

理由：
- AlertGateway 是单进程应用，没有多服务编排、依赖隔离的需求
- 需要直通的硬件设备多（摄像头 V4L2、NPU、MPP 编码器、RGA），容器化会引入设备映射的复杂度，但收益（环境一致性、回滚)对单一固定板子的场景很有限
- 当前 `start.sh` 已经覆盖 start/stop/status/log/restart，够用

如果只是想要"打包"这个动作更规范，可以止步于方案一：WSL 出二进制 → `tools/board_mcp.py` 的 `board_upload` 或 `scp` 上传 → `start.sh` 启动，不需要板子上也跑 Docker。

### 备选：板子也用 Docker（如果有多板分发/统一版本管理的需求）

#### 镜像内容
- 基础镜像需与板子系统的 glibc/内核版本匹配（板子是 Firefly ROC-RK3588S-PC，需确认具体发行版后选对应 `arm64v8/<distro>` 基础镜像）
- 镜像内只装可以走标准 apt 源的运行时依赖（如 `libpaho-mqtt3c`）
- **不要**把 `librknnrt.so`、`librockchip_mpp.so` 打进镜像 —— 这两个库强绑定板子内核驱动版本，应该挂载板子本机路径，而不是固化在镜像里：
  ```
  -v /usr/lib/aarch64-linux-gnu/librknnrt.so:/usr/lib/aarch64-linux-gnu/librknnrt.so:ro
  -v /usr/lib/aarch64-linux-gnu/librockchip_mpp.so:/usr/lib/aarch64-linux-gnu/librockchip_mpp.so:ro
  ```

#### 设备直通
需要确认的设备节点（建议实施前先在板子上 `ls -la /dev` 和 `dmesg | grep -iE 'rknpu|mpp|rga'` 核实实际节点名）：

| 用途 | 设备节点（待核实） | 对应 docker 参数 |
|------|------|------|
| 摄像头（V4L2） | `/dev/video20` | `--device /dev/video20` |
| NPU 推理 | 可能无独立节点，走 `/dev/dri/*` 或 `dma-buf` | 需核实 |
| MPP 硬编码 | `/dev/mpp_service` 等 | 需核实 |
| RGA 硬件预处理 | `/dev/rga` | `--device /dev/rga` |
| DMA 缓冲区分配 | `/dev/dma_heap/*` | 需核实 |

建议路径：先用 `--privileged` 跑通整条 pipeline（采集→推理→编码→推流→MQTT），确认功能正常后再逐步替换成精确的 `--device` 列表，收紧权限。

#### 网络
RTMP 推流和 MQTT 上报都是出站连接，建议直接 `--network host`，省去端口映射（边缘设备场景不需要跟其他容器隔离端口）。

---

## 小结：建议的落地顺序

1. 先做**方案一**（WSL 构建容器化），风险低、收益明确（环境可复现）
2. 板子端先**维持裸机部署**，除非有明确的多板分发/版本管理诉求
3. 如果确实要做板子端 Docker 化，先核实设备节点清单，用 `--privileged` 验证一遍硬件直通可行性，再收紧权限
