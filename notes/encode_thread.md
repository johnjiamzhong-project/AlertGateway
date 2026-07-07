# 话题:EncodeThread 类设计重点

记录格式约定:核心一句话 / 整体骨架 / 要点N / 注意事项 / 代码位置参考

## 核心一句话
这个类最适合面试展开的有四块:跟InferThread解耦的设计动机(详见 [infer_encode_split.md](infer_encode_split.md))、手写NV12像素操作没用图形库、MPP的引用计数式buffer管理、CBR+GOP的码率选型权衡;其中"引用计数buffer"是最容易被问混的点,跟InferThread"谁分配谁释放"的简单模型完全不同。
这个类最适合面试展开的有四块:跟InferThread解耦的设计动机(详见 [infer_encode_split.md](infer_encode_split.md))、RGA硬件加速与零拷贝设计、MPP的引用计数式buffer管理、CBR+GOP的码率选型权衡;其中"引用计数buffer"是最容易被问混的点,跟InferThread"谁分配谁释放"的简单模型完全不同。

## 整体五步骨架(回答"详细讲下run()"时用)
```
取一帧 → 转格式 → 叠检测框 → 喂给硬件编码器 → 取出编码结果
```
1. **取一帧**:`in_queue_.pop(frame, 200)` 阻塞取一帧YUYV422,200ms超时定期检查`running_`。跟InferThread不同,这里**不排空积压帧**——`enc_queue`容量只有1本身就是背压点,必须按顺序逐帧编码保流畅,不能跳帧。
2. **转格式**:`rga_yuyv_to_nv12`, RGA 硬件加速优先（YUYV422→NV12）, 失败时 CPU 软转 `yuyv_to_nv12` 兜底（见要点1）。
3. **叠检测框**:`shared_dets_.get()`读最新一次NPU推理结果(可能不是当前帧的,15fps下无感知)→`draw_rect_nv12`直接在映射出的 DRM Buffer 像素上画框。
4. **喂给硬件编码器**:`mpp_buffer_get` 从 DRM pool 获取缓存指针 -> 直接作为 RGA 转换和画框的目标（Zero-Copy） -> 包装成 `MppFrame` -> `encode_put_frame` 提交。避免了额外的 `memcpy`（见要点2）。
5. **取出编码结果**:`while(encode_get_packet(...))`循环取干净(put/get是异步流水线关系,见要点3),每个包标记是否关键帧,push到`stream_queue`给`StreamThread`。

循环最后还有个每10秒打印一次实际编码帧率的统计,是调试旁路代码,跟主流程没有强耦合。

## 要点1:RGA 硬件加速与 CPU 兜底设计
使用 RGA 2D 加加速器直接完成 `YUYV` 到 `NV12` 的格式转换，结果直接写入 MPP 的 DRM Buffer（零拷贝）。
- 兜底设计：当 RGA 失败时，回退到 CPU 软转（`yuyv_to_nv12`）并进行 `memcpy`。
- 画框操作：`draw_rect_nv12` 直接操作 DRM 映射出的物理缓存（`drm_ptr`），在 Y/UV 分量上进行画线，无需借助 OpenCV。
- 追问：为什么在 DRM Buffer 上画框？——如果先在 CPU 内存画框再拷贝，会多一次 memcpy。直接在 DRM 映射地址画框可以完全实现 zero-copy 流水线。
- 配图:`docs/NV12渲染原理图解.html` 用交互式像素网格可视化了"2×2像素共享一组UV"+`y>>1`/`x&~1`这两行怎么算出UV平面字节偏移量，对着代码看不直观时打开这个文件看一眼比看文字快。

## 要点2:MPP的引用计数式buffer管理(容易问混的点)
```cpp
mpi_->encode_put_frame(ctx_, mpp_frame);   // 喂给编码器,编码器内部自己抓一份buffer引用
mpp_frame_deinit(&mpp_frame);              // 销毁的只是元数据描述符,不是buffer本体
mpp_buffer_put(frame_buf);                 // 释放调用方自己持有的那份buffer引用
```
- 关键理解:`MppFrame`只是个轻量描述符(宽高/格式/PTS+指向buffer的指针),`MppBuffer`才是真正的DRM物理内存,两者生命周期不同。
- `encode_put_frame`提交时,编码器会自己对`frame_buf`加一份引用计数;调用方这边`mpp_buffer_put`只是减掉自己的那份引用,不代表内存立刻被释放——只有编码器内部处理完、也释放了它那份引用,buffer才真正归还给`buf_group_`池子。
- 追问:这样写不会有数据竞争/提前释放的风险吗?——不会,这正是引用计数的意义:谁用谁加引用,用完自己减引用,内存什么时候真正释放由计数归零决定,不是由某一方说了算。

## 要点3:put/get是异步流水线关系,要用while取干净
`encode_put_frame`喂一帧不保证立刻对应一次`encode_get_packet`能取到包——硬件内部有缓冲,可能这次put还没产出包,也可能攒了多个包一起能取。所以取包要用`while (encode_get_packet(...) == MPP_OK && packet)`循环取干净(line 227-246),否则会有包积压、时间戳错位的问题。
- 每个包会从`MppMeta`里读`KEY_OUTPUT_INTRA`判断是否关键帧(line 233-237),标记给下游`StreamThread`用——新观众接入RTMP流必须先等到一个关键帧才能正常解码。

## 要点4:CBR+GOP=8 的码率选型权衡
- 码率模式选CBR(恒定码率)而不是VBR,因为是RTMP直播推流场景,带宽要稳定;VBR能在复杂场景给更好画质,但这不是优先级。
- GOP=8(在14.6fps下约550ms一个关键帧):GOP小意味着关键帧密集,抗丢包、方便随机访问(新观众接入更快看到关键帧),代价是码率开销变大;这是低帧率+实时性优先场景下的选择,不是普适最优值。

## 要点5:跟InferThread解耦的设计动机
详见 [infer_encode_split.md](infer_encode_split.md),核心是NPU推理速度跟摄像头出帧速度不匹配(设计成型时FP16阶段NPU总耗时~150ms,远慢于摄像头~67ms/帧;INT8优化后已降到~48ms,跟摄像头帧率接近),靠`SharedDetections`这种覆盖式共享区解耦两者节奏,代价是编码画的框可能是上一次推理的结果。数字会过时,但解耦带来的并行执行和故障隔离这两条理由跟具体耗时数字无关,设计依然成立。

## 注意事项
1. 要点2(引用计数buffer)最容易被问混,一定要讲清楚"MppFrame描述符"和"MppBuffer物理内存"是两个不同生命周期的对象,不要笼统说成"释放了"。
2. 要点1里如果被追问"为什么使用 RGA 及零拷贝"，可以从省去 460KB memcpy 的 CPU 额外开销，以及减轻对 NPU 推理耗时的 CPU governor 干扰来阐述优化收益。
3. 先给核心一句话结论,等面试官追问再展开到具体代码行。

## 代码位置参考
`src/encode/EncodeThread.cpp`
- init_mpp: mpp_create+mpp_init, MppEncCfg参数提交+释放, DRM buffer group
- rga_yuyv_to_nv12 (line 12-27), yuyv_to_nv12 软转换兜底 (line 110-128), draw_hline/vline/rect_nv12 (line 137-178)
- 配套可视化:`docs/NV12渲染原理图解.html`(像素坐标→UV平面偏移量的交互演示)
- run: RGA 硬件转换并直接写入 DRM buffer，失败则 CPU fallback 拷贝 (line 212-226)，put_frame+引用计数释放 (line 237-246), get_packet循环取干净 (line 249-268)
