# 话题:StreamThread 类设计重点

记录格式约定:核心一句话 / 要点N / 注意事项 / 代码位置参考

## 核心一句话
这个类是流水线终点(消费stream_queue推RTMP流),最适合展开的有四块:为什么用H.264不用H.265(协议层硬约束)、Annex B↔AVCC两种H.264封装格式的转换、SPS/PPS缓存支撑快速重连、双重断线检测机制;跟前两个线程比,这里的"协议细节"(NALU解析、容器封装)是最容易被问出深度的部分。

## 要点1:为什么用H.264不用H.265
FLV/RTMP协议规范本身没有给HEVC分配标准的视频编码器ID,标准RTMP生态(SRS服务器、通用播放器)默认只认H.264;某些平台有私有"Enhanced RTMP"扩展支持HEVC,但不是通用标准,这个项目用的是标准SRS+自写播放器,没理由折腾不通用的路子。
- 不是硬件做不到:项目早期阶段(`docs/第五阶段-多媒体硬解完成记录.md`)验证过板子MPP硬件H.264/H.265编解码都支持,选H.264纯粹是协议兼容性+场景考虑,不是能力不够。
- 场景上也不需要H.265:H.265最大优势是同画质省30-50%码率,这在带宽紧张场景(移动网络/CDN成本)最有价值;这个项目是局域网内推流,带宽不是瓶颈,反而H.265编码更复杂可能增加延迟,跟项目"端到端延迟<1秒"的目标方向相反。
- 代码佐证:`StreamThread.cpp`(`codec_id = AV_CODEC_ID_H264`)和`EncodeThread.cpp`(`MPP_VIDEO_CodingAVC`)两处都硬编码H.264,不是临时漏配。

## 要点2:Annex B ↔ AVCC,两种H.264封装格式的转换
EncodeThread/MPP吐出来的是**Annex B**格式(每个NALU前用起始码`00 00 01`/`00 00 00 01`分隔,无显式长度),但FLV容器要求**AVCC**格式(4字节长度前缀,SPS/PPS单独抽出存进extradata,不跟着每帧重复携带)。
- `parse_annexb()`:扫描起始码切出NALU列表,每个NALU的`type`取自第一个字节低5位(`&0x1F`),7=SPS,8=PPS(H.264标准固定编号)。
- `write_extradata()`:把SPS/PPS打包成`AVCDecoderConfigurationRecord`标准结构(`0x01`版本号+profile/level+`0xFF`/`0xE1`这两个标志字节+SPS/PPS各自的长度前缀)塞进`extradata`,触发`avformat_write_header`——播放端解码前必须先读到这个才知道分辨率/profile等参数。
- `write_packet()`:把图像NALU(跳过SPS/PPS)重新拼成4字节长度前缀的AVCC格式逐帧写出去。
- 一句话总结:这几个函数本质是在手动实现"H.264↔FLV容器"的协商协议,AVCC这些封装规范是H.264专属的,这也是H.265在标准FLV/RTMP里走不通的具体技术细节(没有等价的封装方式)。

## 要点3:SPS/PPS缓存,支撑断线快速重连
MPP编码器只在关键帧里带SPS/PPS,第一次遇到关键帧时用`sps_.assign(n.data, n.data+n.size)`缓存下来(`assign`是清空+按范围重新填充,等价于"用这段数据整体替换内容")。
- 断线重连(`reconnect_loop()`)时不用等下一个关键帧重新出现,直接拿缓存值立刻`write_extradata`写header,恢复更快。
- `close_rtmp()`特意不清空`sps_`/`pps_`这两个缓存,就是为了支撑这个设计——追问:为什么不清?——因为SPS/PPS在一次推流过程中基本不变(除非分辨率/编码参数变了),旧缓存依然有效,清掉反而要多等一个关键帧才能恢复推流。

## 要点4:双重断线检测机制
- 一是`write_packet()`写失败直接触发`reconnect_loop()`。
- 二是`run()`里的**心跳检测**:如果队列连续5秒没有新包(EncodeThread那边可能暂停推包),主动检查`fmt_ctx_->pb->error`标志——单靠"写失败才重连"在"长时间没东西可写"的场景下会迟迟发现不了网络已经断开。
- 细节:`avio_flush()`之后才检查error字段,因为`av_write_frame`可能仍返回0(数据进了TCP缓冲区还没真正发送),flush才会触发真正发送并暴露`EPIPE`/`ECONNRESET`。

## 要点5:低延迟调优 + 线程对象生命周期
- `AVFMT_FLAG_FLUSH_PACKETS`+`max_delay=0`关掉FFmpeg内部重排缓冲,`rtmp_buffer_size=0`关掉RTMP客户端缓冲——能多快发就多快发,不为流畅攒包。
- `thread_ = std::thread(&StreamThread::run, this)`传的是对象指针不是对象本体,新线程靠`this`访问成员;这要求对象在线程退出前不能被销毁,所以`stop()`必须先`join()`再释放资源,`~StreamThread()`兜底调`stop()`也是同样考虑。

## 注意事项
1. 要点1(H.264 vs H.265)容易被追问"协议为什么不支持",答案要落在"FLV规范没有官方编码器ID"这个具体事实上,不要只说"不兼容"这种空泛结论。
2. 要点2的AVCC/Annex B转换是这个类技术含量最高的部分,讲清楚"为什么需要转换"(容器要求不同)比讲"怎么转换"(位运算细节)更重要。
3. 先给核心一句话结论,等面试官追问再展开到具体代码行。

## 代码位置参考
`src/stream/StreamThread.cpp`
- open_rtmp: 低延迟参数配置 (line 26-54)
- write_extradata: AVCDecoderConfigurationRecord打包 (line 81-111)
- parse_annexb: Annex B起始码扫描 (line 113-142)
- write_packet: SPS/PPS缓存+AVCC组包 (line 144-192)
- reconnect_loop: 断线重连复用缓存SPS/PPS (line 194-212)
- run: 主循环+心跳检测 (line 214-239)
