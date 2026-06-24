# 话题:CaptureThread::run() 完整数据流

记录格式约定:核心一句话 / 易错点N / 五步骨架 / 代码位置参考

## 核心一句话
run() 每轮循环是"等帧→取帧→封装→分发→还帧"五步,循环节奏由摄像头出帧速度决定(select阻塞等待),不是代码跑多快;分发给两个下游队列时,一个是真拷贝,一个是move,不要笼统说成"复制"。

## 易错点1:select 不是"取数据",是"等信号"
`select(fd_+1, &fds, ...)` 只检查 `fd_` 是否进入可读状态(内核有没有填好缓冲区),本身不读取任何图像数据,返回的只是"有/没有/超时"状态。真正从内核取数据的是接下来的 `VIDIOC_DQBUF`。
- 准确表述:"select 阻塞等待摄像头fd可读,确认有新帧后,再用 VIDIOC_DQBUF 从内核取出已填好的缓冲区。"——"等"和"取"是两个独立步骤,不能合并说。

## 易错点2:发往两个队列不全是"复制"
```cpp
Frame infer_copy = frame;
infer_queue_.push(std::move(infer_copy), 0);   // 先深拷贝一份,再move进队列
enc_queue_.push(std::move(frame), 100);        // 直接move原对象,无额外拷贝
```
- 给 `infer_queue` 的是先深拷贝一份(`raw_data`是vector,这一步发生真正内存复制),因为后面 `enc_queue` 还要用同一份数据。
- 给 `enc_queue` 的是直接move原对象(转移vector的指针所有权,不拷贝内存),因为这是这份数据的最后一次使用。
- 设计逻辑:只有"还需要再用一次"的数据才真拷贝,能转移所有权的地方就用move省一次拷贝。

## 易错点3:循环节奏由硬件帧率决定,不是CPU跑多快
循环体本身的代码执行(DQBUF→memcpy→push×2→QBUF)是亚毫秒级,很快;但 `select` 会阻塞到摄像头产生下一帧为止(比如14.6fps对应约68ms一帧),所以循环的实际节奏≈摄像头出帧间隔,不是"代码越快循环越快"。这是阻塞IO模型省CPU的体现,不是busy polling。

## 易错点4:CaptureThread 的下游只有 InferThread 和 EncodeThread 这两条
分发的两个队列(`infer_queue`/`enc_queue`)分别接到 `InferThread` 和 `EncodeThread`,这条数据流到这里就分叉了,`CaptureThread` 不再参与后续任何环节。
- 容易讲错的地方:`InferThread` 推理完的检测结果,**不会**回流给 `CaptureThread`,也**不会**排队传给 `EncodeThread`——它写进的是 `SharedDetections` 这个共享区,`EncodeThread` 自己去读最新一份叠框(详见`infer_encode_split.md`/`main_orchestration.md`拓扑图)。这是跟两条队列并列的一条旁路连接,不要把它也算成"CaptureThread分发出去的第三条线"。

## 五步骨架(回答"详细讲下run()"时用)
1. select 阻塞等待 fd 可读(超时2s,用于响应stop()标志)
2. VIDIOC_DQBUF 从内核取出已填好的缓冲区,得到 buf.index
3. 用 buf.index 找到对应mmap地址,memcpy进 Frame::raw_data(唯一一次拷贝,必须在QBUF归还前完成)
4. 分发:深拷贝一份给infer_queue(非阻塞push,timeout=0,忙时丢帧);move原对象给enc_queue(阻塞至多100ms,唯一背压点)
5. VIDIOC_QBUF 把缓冲区还给内核,供下一帧复用

## 代码位置参考
`src/capture/CaptureThread.cpp` run() (line 36-78)
- select (line 39-46)
- DQBUF (line 49-53)
- Frame封装/memcpy (line 55-64)
- 分发到两队列 (line 66-73)
- QBUF归还 (line 76)
