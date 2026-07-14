# AlertGateway 项目规则与开发指南

本文档定义了 AlertGateway 项目的开发规范、编译指令和板端部署流程，供 AI 助手（Antigravity）在后续开发中严格遵守。

## 0. 会话启动与增量推进规则

* 每次开始处理本项目之前，必须先完整阅读项目根目录的 `PROJECT_MEMORY.md`，再检查与当前任务直接相关的文件和 Git 状态。
* 后续工作必须基于 `PROJECT_MEMORY.md` 记录的当前状态、既有决定和下一步增量推进，不得无视已有实现重新设计整个项目。
* 完成重要实验、改变技术路线、处理完既有待办，或者发现记忆内容已经过时时，必须同步更新 `PROJECT_MEMORY.md`。
* `PROJECT_MEMORY.md` 只保存简明的项目事实、决定和当前方向；详细实验过程写入 `docs/` 下对应文档，避免把记忆文件变成长篇流水账。
* 如果 `PROJECT_MEMORY.md` 与源码、Git状态或最新实测结果冲突，以可验证的最新事实为准，并在本次工作中修正记忆文件。

## 1. 项目概述与核心架构
AlertGateway 是运行在 RK3588 平台的智能边缘网关，用于实时摄像头流采集、NPU 检测与视频编码推流。
* **开发语言**：C++11/17，基于 CMake 构建系统。
* **核心线程**：
  * `CaptureThread`：基于 V4L2 采集摄像头 YUYV/NV12 原始数据。
  * `InferThread`：基于 NPU (RKNN API) 进行 Yolov8 目标检测。
  * `EncodeThread`：基于 Rockchip MPP 进行硬件 H.264/H.265 视频编码。
* **硬件加速原则**：优先使用 MPP（编码）与 RGA（图像缩放/格式转换）硬件加速，并保留 CPU 兜底逻辑。

## 2. 编译指南 (Host 主机)
项目采用交叉编译，目标平台为 aarch64 (Linux)。

### 2.1 编译命令
在项目根目录下执行以下命令进行配置与编译：
```bash
# 配置交叉编译 (指定 RKNN 库路径与 aarch64 工具链)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake -DRKNN_LIB_PATH=/home/rambos/arm_test/AlertGateway/third_party/rknn

# 执行编译
cmake --build build
```

## 3. 板端部署与调试 (RK3588 开发板)
开发板默认 IP 地址：`192.168.0.200` 或 `192.168.0.201`，登录账号：`firefly`。

### 3.1 部署命令
在主机编译成功后，可运行以下命令同步至开发板：
```bash
# 同步主程序
scp build/AlertGateway firefly@192.168.0.200:~/AlertGateway/

# 同步配置文件
scp config/config.json firefly@192.168.0.200:~/AlertGateway/config/
```

### 3.2 系统调优规范
* 确保板端 CPU/NPU Governor 固定为 `performance` 以保证 NPU 推理耗时稳定在 ~31ms。
* 项目在 `tools/performance/` 提供了开机自启该配置的 systemd 服务，在 Host 主机上直接执行 [tools/performance/deploy.sh](file:///home/rambos/arm_test/AlertGateway/tools/performance/deploy.sh) 即可一键向板子部署并启动。

## 4. Git 提交与推送规范
* **非主动提交原则**：在任何情况下，AI 助手**绝对不能**在没有得到用户明确授权或明确指示的前提下，擅自执行 `git commit` 或 `git push` 命令。所有产生的修改应保持在工作区（staged 或 unstaged）状态，等待用户自行决定并执行提交。
* **提交信息语言**：本项目的 `git commit` message 必须使用中文，标题和正文均应清晰说明本次变更内容。
