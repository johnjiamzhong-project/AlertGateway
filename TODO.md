# AlertGateway 优化待办

记录已发现但还没做、或还没实测验证收益的优化点。这里是项目正式维护的工程待办。

状态说明：`[ ]` 未做 / `[~]` 已验证方向可行但未实施 / `[x]` 已完成（完成后请移到对应模块说明或 `docs/`，并从本文件删除）。

---

## EncodeThread

- [ ] **`yuyv_to_nv12` 改用 RGA 硬件加速**，替换当前纯CPU逐像素转换。`InferThread` 里同样的 YUYV 转换场景（`rga_yuyv_to_rgb_resize`）已经验证 RGA 硬件可用且是"硬件优先+CPU兜底"模式，`EncodeThread::yuyv_to_nv12`（`src/encode/EncodeThread.cpp`）目前只有CPU路径，是架构不一致。预期收益：省CPU占用，可能减轻对NPU推理耗时的CPU governor干扰（见下一条）。尚未做实测对比。
- [ ] **`run()` 里 memcpy 进 DRM buffer 这一次拷贝是否能省掉**：现在是 `yuyv_to_nv12` 先转换到本地 `nv12_buf`，再 `memcpy` 进 MPP 的 DRM buffer（`src/encode/EncodeThread.cpp`，`mpp_buffer_get` 之后那行）。理想情况下可以让转换结果直接写入 MPP buffer，省掉这次拷贝，但要看 V4L2 采集端的内存模型是否方便配合，待确认可行性。


## 模型量化实验（项目延伸，优先级待定）

- [ ] **校准算法对比**：`rknn-toolkit2` 转换脚本（`tools/convert_int8.py`）里把量化校准算法依次换成 `normal`/`mmse`/`kl_divergence`，其余配置不变，各跑一次转换；对比项：检测精度（同一批测试图片的mAP或人工抽查漏检/误检数量）+ 板子实测 `rknn_run` 耗时。预计1天内可完成，产出一张对比表。
- [ ] **混合精度量化**：box回归分支保留更高精度（FP16或更高量化档位），其余维持INT8（`rknn-toolkit2` 支持按层/算子名指定量化精度）；对比"全INT8" vs "box分支高精度+其余INT8"两版的精度/耗时差异。直接延伸自 `BUGS.md` BUG-009（box/cls共享scale导致精度丢失）的真实排查经历，用数据支撑"为什么box层对量化敏感"这个论点。

QAT（量化感知训练）和剪枝暂不列入：QAT需要完整重训练YOLOv8s，本地GPU（GTX 1650S）跑不动且性价比低；剪枝（torch-pruning结构化剪枝+真实NPU收益测试）是新开的独立方向，待上面两项做完且时间允许再评估。

## 其他工具（非 AlertGateway 主流程）

- [ ] **WebRTC 远程桌面方案延迟 1-2 秒**：板子上 GStreamer+WebRTC 远程桌面工具（跟 AlertGateway 检测/推流业务无关，是独立的开发便利性工具），原因是 webrtcbin jitter buffer + gop=30，待优化。详见 `docs/WebRTC远程桌面方案.md`。

---

完成某项后：在对应代码处补充/更新注释说明已采用的方案，并从本文件删除该条目（必要时连同验证数据写一篇 `docs/` 记录）。
