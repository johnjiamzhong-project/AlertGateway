# AlertGateway 优化待办

记录已发现但还没做、或还没实测验证收益的优化点。这里是项目正式维护的工程待办。

状态说明：`[ ]` 未做 / `[~]` 已验证方向可行但未实施 / `[x]` 已完成（完成后请移到对应模块说明或 `docs/`，并从本文件删除）。

---

## EncodeThread

- [ ] **`yuyv_to_nv12` 改用 RGA 硬件加速**，替换当前纯CPU逐像素转换。`InferThread` 里同样的 YUYV 转换场景（`rga_yuyv_to_rgb_resize`）已经验证 RGA 硬件可用且是"硬件优先+CPU兜底"模式，`EncodeThread::yuyv_to_nv12`（`src/encode/EncodeThread.cpp`）目前只有CPU路径，是架构不一致。预期收益：省CPU占用，可能减轻对NPU推理耗时的CPU governor干扰（见下一条）。尚未做实测对比。
- [ ] **`run()` 里 memcpy 进 DRM buffer 这一次拷贝是否能省掉**：现在是 `yuyv_to_nv12` 先转换到本地 `nv12_buf`，再 `memcpy` 进 MPP 的 DRM buffer（`src/encode/EncodeThread.cpp`，`mpp_buffer_get` 之后那行）。理想情况下可以让转换结果直接写入 MPP buffer，省掉这次拷贝，但要看 V4L2 采集端的内存模型是否方便配合，待确认可行性。

## NPU / 系统调优

- [ ] **CPU/NPU governor 固定为 `performance`（持久化配置）**：实测发现 governor 设为 `performance` 时单帧 NPU 推理（`rknn_run`）可压到 ~31ms，真实运行时（governor 按需调频）在 32-51ms 间波动。目前只是 sysfs 临时生效，重启会还原，是否做成持久化配置（开机脚本/systemd）待定。用更高功耗换取更低且更稳定的延迟抖动，需要权衡功耗预算。详见 `docs/NPU官方demo测速对比记录.md`。

## 其他工具（非 AlertGateway 主流程）

- [ ] **WebRTC 远程桌面方案延迟 1-2 秒**：板子上 GStreamer+WebRTC 远程桌面工具（跟 AlertGateway 检测/推流业务无关，是独立的开发便利性工具），原因是 webrtcbin jitter buffer + gop=30，待优化。详见 `docs/WebRTC远程桌面方案.md`。

---

完成某项后：在对应代码处补充/更新注释说明已采用的方案，并从本文件删除该条目（必要时连同验证数据写一篇 `docs/` 记录）。
