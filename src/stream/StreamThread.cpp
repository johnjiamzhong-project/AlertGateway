#include "stream/StreamThread.hpp"
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <chrono>

const char* stream_sink_name(StreamSinkType sink) {
    switch (sink) {
    case StreamSinkType::FixedRtmp: return "fixed_rtmp";
    case StreamSinkType::LocalFlv: return "local_flv";
    case StreamSinkType::MetricsOnly: return "metrics_only";
    }
    return "unknown";
}

namespace {

std::string channel_tag(const StreamConfig& cfg) {
    return "[channel=" + cfg.channel_id + "] ";
}

}  // namespace

// 只保存引用/配置，不做网络/FFmpeg初始化——真正连接推迟到 start()。
StreamThread::StreamThread(const StreamConfig& cfg, BlockingQueue<EncodedPacket>& in_queue)
    : cfg_(cfg), in_queue_(in_queue) {}

// 析构兜底调用 stop()，保证线程和RTMP连接不会泄漏。
StreamThread::~StreamThread() { stop(); }

// 由 main 调用一次：先初始化FFmpeg网络子系统(全局只需一次)，再建立首次RTMP连接；
// 连接失败直接抛异常终止启动——推流是流水线终点，连不上没有降级方案。
// 成功后置 running_=true 并起 run() 所在的工作线程。
void StreamThread::start() {
    if (cfg_.sink == StreamSinkType::FixedRtmp) {
        int ret = avformat_network_init();
        if (ret < 0)
            throw std::runtime_error("StreamThread: avformat_network_init failed");
    }
    if (cfg_.sink != StreamSinkType::MetricsOnly && !open_rtmp()) {
        throw std::runtime_error("StreamThread: failed to open " + cfg_.rtmp_url);
    }
    running_ = true;
    thread_ = std::thread(&StreamThread::run, this);
}

// 置 running_=false 让 run() 循环下一次检查时自行退出，join 等待线程真正结束，
// 再关闭RTMP连接（写trailer、释放FFmpeg资源）。
void StreamThread::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    close_rtmp();
}

// 创建一个flv格式的输出上下文并连上RTMP服务器，只配视频流（H.264），不含音频。
// 几个关键配置都是为了把端到端延迟压到最低：关掉重排缓冲、关掉RTMP客户端缓冲，
// 数据能多快发出去就多快发出去，不为了画面更流畅而攒包。
bool StreamThread::open_rtmp() {
    AVFormatContext* ctx = nullptr;
    int ret = avformat_alloc_output_context2(&ctx, nullptr, "flv",
                                              cfg_.rtmp_url.c_str());
    if (ret < 0 || !ctx) return false;

    // 立即刷新每个包，不做重排缓冲
    ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    ctx->max_delay = 0;

    AVStream* st = avformat_new_stream(ctx, nullptr);
    if (!st) { avformat_free_context(ctx); return false; }

    st->codecpar->codec_type  = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id    = AV_CODEC_ID_H264;
    st->codecpar->width       = cfg_.width;
    st->codecpar->height      = cfg_.height;
    st->time_base             = AVRational{1, cfg_.fps};

    AVDictionary* opts = nullptr;
    if (cfg_.sink == StreamSinkType::FixedRtmp) {
        av_dict_set_int(&opts, "rtmp_buffer_size", 0, 0);  // 禁用 RTMP 客户端缓冲
    }
    ret = avio_open2(&ctx->pb, cfg_.rtmp_url.c_str(), AVIO_FLAG_WRITE, nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) { avformat_free_context(ctx); return false; }

    fmt_ctx_  = ctx;
    video_st_ = st;
    return true;
}

void StreamThread::close_rtmp() {
    if (fmt_ctx_) {
        if (header_written_) av_write_trailer(fmt_ctx_);
        if (fmt_ctx_->pb)    avio_closep(&fmt_ctx_->pb);
        avformat_free_context(fmt_ctx_);
        fmt_ctx_  = nullptr;
        video_st_ = nullptr;
        header_written_ = false;
        extradata_set_  = false;
        pkt_idx_ = 0;
        // sps_/pps_ 保留：MPP 编码器只在首帧关键帧带 SPS/PPS，
        // 重连后直接用缓存值写 header，不用等下一个关键帧
    }
}

// 把 SPS/PPS 编码成 AVCDecoderConfigurationRecord 格式塞进 extradata，
// 这是 AVCC（MP4/FLV系容器用的H.264封装方式）描述"怎么解码这条流"的标准头部，
// 播放端/SRS服务器靠这个头部知道分辨率/profile等参数，没有这个数据就解不出画面。
// 写完 extradata 立刻调 avformat_write_header()——FLV容器的header只能写一次，
// 必须先有SPS/PPS才能写，所以这个函数也承担了"触发写header"的职责。
bool StreamThread::write_extradata(const uint8_t* sps, size_t sps_len,
                                    const uint8_t* pps, size_t pps_len) {
    // AVCDecoderConfigurationRecord 固定头部11字节(version/profile/level/NALU长度字段宽度等)
    // + SPS数据(带2字节长度前缀) + PPS数据(带2字节长度前缀)
    size_t extra_size = 11 + sps_len + pps_len;
    auto* extra = static_cast<uint8_t*>(av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!extra) {
        std::cerr << "StreamThread: av_mallocz extradata failed\n";
        return false;
    }
    size_t p = 0;
    extra[p++] = 0x01;                                            // configurationVersion=1
    extra[p++] = sps[1]; extra[p++] = sps[2]; extra[p++] = sps[3]; // profile/兼容性/level，直接抄SPS里的
    extra[p++] = 0xFF;  // 0b11111111：高6位reserved全1 + lengthSizeMinusOne=3，即NALU长度前缀是4字节
    extra[p++] = 0xE1;  // 0b11100001：高3位reserved全1 + numOfSPS=1
    extra[p++] = (sps_len >> 8) & 0xFF; extra[p++] = sps_len & 0xFF;  // SPS长度(2字节大端)
    memcpy(extra + p, sps, sps_len); p += sps_len;
    extra[p++] = 0x01;  // numOfPPS=1
    extra[p++] = (pps_len >> 8) & 0xFF; extra[p++] = pps_len & 0xFF;  // PPS长度(2字节大端)
    memcpy(extra + p, pps, pps_len);

    video_st_->codecpar->extradata      = extra;
    video_st_->codecpar->extradata_size = static_cast<int>(extra_size);
    extradata_set_ = true;

    int ret = avformat_write_header(fmt_ctx_, nullptr);
    if (ret < 0) return false;
    header_written_ = true;
    return true;
}

// EncodeThread/MPP 吐出来的H.264码流是 Annex B 格式：每个NALU(网络抽象层单元，
// 一个完整的编码单元，比如一帧、或者SPS/PPS这种参数集)前面用"起始码"
// (00 00 01 三字节 或 00 00 00 01 四字节)分隔，没有显式长度字段，
// 必须扫描找起始码才能切出每个NALU的边界——这个函数就是干这个的。
std::vector<StreamThread::Nalu>
StreamThread::parse_annexb(const uint8_t* buf, size_t len) const {
    std::vector<Nalu> nalus;
    size_t i = 0;
    while (i < len) {
        // 找起始码，优先匹配4字节(00 00 00 01)，否则匹配3字节(00 00 01)
        int sc = 0;
        if (i+4 <= len && buf[i]==0 && buf[i+1]==0 && buf[i+2]==0 && buf[i+3]==1) sc = 4;
        else if (i+3 <= len && buf[i]==0 && buf[i+1]==0 && buf[i+2]==1) sc = 3;
        else { i++; continue; }  // 没匹配上，往后挪一个字节继续找

        // 从这个NALU的起始码之后开始，往后扫到下一个起始码（或buf结尾）就是它的结束位置
        size_t start = i + sc;
        size_t end   = len;
        for (size_t j = start; j + 3 <= len; j++) {
            if (buf[j]==0 && buf[j+1]==0 &&
                (buf[j+2]==1 || (j+4<=len && buf[j+2]==0 && buf[j+3]==1))) {
                end = j; break;
            }
        }
        // NALU第一个字节的低5位(&0x1F)就是nal_unit_type：7=SPS, 8=PPS, 5=IDR帧, 1=普通帧 等
        if (end > start)
            nalus.push_back({buf + start, end - start, static_cast<uint8_t>(buf[start] & 0x1F)});
        i = end;
    }
    return nalus;
}

// 把一个 EncodedPacket（一帧H.264数据，可能包含多个NALU）转成RTMP/FLV要的格式写出去。
// 流程：拆出NALU → 第一次遇到关键帧时顺带把SPS/PPS缓存下来并写header →
// 把视频帧数据(去掉SPS/PPS,它们已经进了extradata)重新拼成AVCC格式 → 调FFmpeg写帧。
bool StreamThread::write_packet(const EncodedPacket& ep) {
    auto nalus = parse_annexb(ep.data.data(), ep.data.size());

    // 只有关键帧才会带SPS/PPS（MPP编码器的行为），还没设置过extradata时才需要找一次
    if (!extradata_set_ && ep.is_keyframe) {
        for (const auto& n : nalus) {
            if (n.type == 7) sps_.assign(n.data, n.data + n.size);
            if (n.type == 8) pps_.assign(n.data, n.data + n.size);
        }
        if (!sps_.empty() && !pps_.empty()) {
            if (!write_extradata(sps_.data(), sps_.size(), pps_.data(), pps_.size()))
                return false;
        }
    }

    // header没写成功(还没等到关键帧/SPS+PPS)之前先丢弃这帧，不能提前写视频数据
    if (!header_written_) return true;

    // 组AVCC格式的包：把每个NALU前面的Annex B起始码换成4字节大端长度前缀，
    // SPS/PPS(type 7/8)跳过不重复写——它们已经在write_extradata()里进了extradata，
    // 容器header只需要存一次，不需要跟着每个关键帧重复携带
    std::vector<uint8_t> avcc;
    for (const auto& n : nalus) {
        if (n.type == 7 || n.type == 8) continue;
        uint32_t sz = static_cast<uint32_t>(n.size);
        avcc.push_back((sz >> 24) & 0xFF);
        avcc.push_back((sz >> 16) & 0xFF);
        avcc.push_back((sz >>  8) & 0xFF);
        avcc.push_back( sz        & 0xFF);
        avcc.insert(avcc.end(), n.data, n.data + n.size);
    }
    if (avcc.empty()) return true;  // 这帧全是SPS/PPS，没有实际画面数据，不用写

    // pts/dts 用 pkt_idx_ 线性递增(按帧计数，不是按时间)，靠 av_packet_rescale_ts
    // 从 {1,fps} 这个时间基换算成 video_st_->time_base 要求的单位
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        std::cerr << "StreamThread: av_packet_alloc failed\n";
        return false;
    }
    pkt->data  = avcc.data();
    pkt->size  = static_cast<int>(avcc.size());
    pkt->pts   = pkt->dts = pkt_idx_++;
    pkt->flags = ep.is_keyframe ? AV_PKT_FLAG_KEY : 0;
    av_packet_rescale_ts(pkt, AVRational{1, cfg_.fps}, video_st_->time_base);
    int ret = av_write_frame(fmt_ctx_, pkt);
    av_packet_free(&pkt);
    if (ret < 0) return false;
    // avio_flush 才是真正触发 TCP 发送的地方；flush 后检查错误字段
    // SRS 关闭时 av_write_frame 可能仍返回 0（数据进了 TCP 缓冲区），
    // 但 flush 后 pb->error 会被置为 EPIPE/ECONNRESET
    avio_flush(fmt_ctx_->pb);
    return fmt_ctx_->pb->error >= 0;
}

// 断线重连：先彻底关掉旧连接，然后阻塞循环重试open_rtmp()直到连上（或running_变false）。
// 重连成功后不必等下一个关键帧——sps_/pps_是上次缓存的，直接拿来写header，
// 立刻能继续推流。重试间隔里持续排空in_queue_，防止EncodeThread被这边的backpressure卡住。
void StreamThread::reconnect_loop() {
    close_rtmp();
    while (running_) {
        if (open_rtmp()) {
            // 用缓存的 SPS/PPS 立刻写 header，不等下一个关键帧
            if (!sps_.empty() && !pps_.empty()) {
                write_extradata(sps_.data(), sps_.size(), pps_.data(), pps_.size());
            }
            std::cout << channel_tag(cfg_) << "StreamThread: reconnected to "
                      << cfg_.rtmp_url << "\n";
            return;
        }
        std::cerr << channel_tag(cfg_) << "StreamThread: reconnect failed, retry in 3s\n";
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (running_ && std::chrono::steady_clock::now() < deadline) {
            EncodedPacket discard;
            in_queue_.pop(discard, 100);  // 持续排空队列，防止背压
        }
    }
}

// 推流主循环，start() 起的线程跑的就是这个函数，running_ 为 false 时退出。
// 每轮：取一个包→写出去；失败就重连。另外维护一个心跳检测：如果队列长时间没有
// 新包（200ms轮询连续5秒都没等到），主动检查一次AVIO的错误标志——网络断开但
// EncodeThread那边因为某种原因暂时没往这边推包时，单靠"写失败才重连"会迟迟发现不了断线。
void StreamThread::run() {
    using clk = std::chrono::steady_clock;
    auto last_ok = clk::now();
    uint64_t written_packets = 0;
    uint64_t written_bytes = 0;
    uint64_t write_failures = 0;
    auto stats_t = clk::now();

    while (running_) {
        EncodedPacket ep;
        if (!in_queue_.pop(ep, 200)) {
            // 5s 内没有成功写包，检查 AVIO 错误标志
            if (header_written_ && clk::now() - last_ok > std::chrono::seconds(5)) {
                if (fmt_ctx_->pb->error < 0) {
                    std::cerr << channel_tag(cfg_)
                              << "StreamThread: connection lost (heartbeat), reconnecting...\n";
                    reconnect_loop();
                    last_ok = clk::now();
                }
            }
            continue;
        }

        if (cfg_.sink == StreamSinkType::MetricsOnly) {
            ++written_packets;
            written_bytes += ep.data.size();
            last_ok = clk::now();
        } else if (!write_packet(ep)) {
            ++write_failures;
            std::cerr << channel_tag(cfg_) << "StreamThread: write failed, reconnecting...\n";
            reconnect_loop();
            last_ok = clk::now();
        } else {
            ++written_packets;
            written_bytes += ep.data.size();
            last_ok = clk::now();
        }

        const double elapsed = std::chrono::duration<double>(clk::now() - stats_t).count();
        if (elapsed >= 10.0) {
            std::cerr << "[StreamStats] channel=" << cfg_.channel_id
                      << " sink=" << stream_sink_name(cfg_.sink)
                      << " output_fps=" << (written_packets / elapsed)
                      << " bitrate_kbps=" << (written_bytes * 8.0 / elapsed / 1000.0)
                      << " write_fail=" << write_failures
                      << " queue=" << in_queue_.size()
                      << "\n" << std::flush;
            written_packets = written_bytes = write_failures = 0;
            stats_t = clk::now();
        }
    }
}
