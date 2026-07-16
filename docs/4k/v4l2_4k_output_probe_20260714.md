# V4L2 工作流 4K 输出探针（2026-07-14）

## 结论

当前 V4L2 工作流不能仅通过配置把摄像头画面“还原”为 3840×2160 输出。现有实现将
`source.width/height` 同时用于 V4L2 采集、MPP 编码和 RTMP 流描述，没有独立的输出尺寸，
也没有 V4L2 YUYV 到 4K NV12 的放大处理。

## 板端实测

- 板端：RK3588，`/dev/video20`，UVC Camera `046d:081b`。
- V4L2 能力表最高为 1280×960；4K `3840×2160` 不在设备支持列表中。
- 临时配置请求 `3840×2160 YUYV` 时，驱动实际协商为 `1280×960 YUYV`，程序输出
  `V4L2 resolution mismatch` 并拒绝启动；未修改生产配置。
- 临时配置请求 `1280×720 YUYV` 时，程序正常打开摄像头，MPP 日志为
  `PREP_CFG w:h [1280:720]`，推流路径也按 1280×720 工作。驱动实际帧率为 10 FPS，
  即使配置请求 15 FPS 也不会被强制提升。

## 60 秒 V4L2 流程测试

使用 `runs/testsrc2/config_v4l2_720p.json` 在板端运行独立候选二进制约 60 秒：

- V4L2 协商：1280×720、YUYV、10 FPS。
- MPP/RTMP：1280×720；稳定窗口编码和输出约 9.75～9.91 FPS。
- 错误计数：`put_fail=0`、`out_drop=0`、`write_fail=0`，输出队列深度为 0。
- 推理持续运行并产生检测结果，程序收到中断后输出 `Done` 正常退出。
- 退出时 FFmpeg/FLV 的 `Failed to update header` 仅出现在主动停止收尾阶段，未出现在
  稳态推流窗口。

原始日志：`runs/testsrc2/board_v4l2_720p_20260714.log`。

## 后续若要支持

需要把采集尺寸与编码输出尺寸拆开，例如保留 `source_width/source_height`，新增
`output_width/output_height`，并在 EncodeThread 中增加 YUYV→NV12 的 RGA 缩放路径，
同时按输出尺寸映射检测框、设置 MPP 和 RTMP 元数据。这个改动属于新的图像处理链路，
不能通过修改现有 V4L2 配置安全实现。
