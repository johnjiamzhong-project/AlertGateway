#include "stream/StreamThread.hpp"
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <chrono>

StreamThread::StreamThread(const StreamConfig& cfg, BlockingQueue<EncodedPacket>& in_queue)
    : cfg_(cfg), in_queue_(in_queue) {}

StreamThread::~StreamThread() { stop(); }

void StreamThread::start() {
    avformat_network_init();
    if (!open_rtmp())
        throw std::runtime_error("StreamThread: failed to connect " + cfg_.rtmp_url);
    running_ = true;
    thread_ = std::thread(&StreamThread::run, this);
}

void StreamThread::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    close_rtmp();
}

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
    av_dict_set_int(&opts, "rtmp_buffer_size", 0, 0);  // 禁用 RTMP 客户端缓冲
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

// Encode SPS+PPS into AVCodecParameters extradata (AVCC format)
bool StreamThread::write_extradata(const uint8_t* sps, size_t sps_len,
                                    const uint8_t* pps, size_t pps_len) {
    // AVCDecoderConfigurationRecord
    size_t extra_size = 11 + sps_len + pps_len;
    auto* extra = static_cast<uint8_t*>(av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE));
    size_t p = 0;
    extra[p++] = 0x01;
    extra[p++] = sps[1]; extra[p++] = sps[2]; extra[p++] = sps[3];
    extra[p++] = 0xFF;
    extra[p++] = 0xE1;
    extra[p++] = (sps_len >> 8) & 0xFF; extra[p++] = sps_len & 0xFF;
    memcpy(extra + p, sps, sps_len); p += sps_len;
    extra[p++] = 0x01;
    extra[p++] = (pps_len >> 8) & 0xFF; extra[p++] = pps_len & 0xFF;
    memcpy(extra + p, pps, pps_len);

    video_st_->codecpar->extradata      = extra;
    video_st_->codecpar->extradata_size = static_cast<int>(extra_size);
    extradata_set_ = true;

    int ret = avformat_write_header(fmt_ctx_, nullptr);
    if (ret < 0) return false;
    header_written_ = true;
    return true;
}

std::vector<StreamThread::Nalu>
StreamThread::parse_annexb(const uint8_t* buf, size_t len) const {
    std::vector<Nalu> nalus;
    size_t i = 0;
    while (i < len) {
        int sc = 0;
        if (i+4 <= len && buf[i]==0 && buf[i+1]==0 && buf[i+2]==0 && buf[i+3]==1) sc = 4;
        else if (i+3 <= len && buf[i]==0 && buf[i+1]==0 && buf[i+2]==1) sc = 3;
        else { i++; continue; }

        size_t start = i + sc;
        size_t end   = len;
        for (size_t j = start; j + 3 <= len; j++) {
            if (buf[j]==0 && buf[j+1]==0 &&
                (buf[j+2]==1 || (j+4<=len && buf[j+2]==0 && buf[j+3]==1))) {
                end = j; break;
            }
        }
        if (end > start)
            nalus.push_back({buf + start, end - start, static_cast<uint8_t>(buf[start] & 0x1F)});
        i = end;
    }
    return nalus;
}

bool StreamThread::write_packet(const EncodedPacket& ep) {
    auto nalus = parse_annexb(ep.data.data(), ep.data.size());

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

    if (!header_written_) return true;

    // Build AVCC packet (4-byte length prefix, no start codes)
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
    if (avcc.empty()) return true;

    AVPacket* pkt = av_packet_alloc();
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

void StreamThread::reconnect_loop() {
    close_rtmp();
    while (running_) {
        if (open_rtmp()) {
            // 用缓存的 SPS/PPS 立刻写 header，不等下一个关键帧
            if (!sps_.empty() && !pps_.empty()) {
                write_extradata(sps_.data(), sps_.size(), pps_.data(), pps_.size());
            }
            std::cout << "StreamThread: reconnected to " << cfg_.rtmp_url << "\n";
            return;
        }
        std::cerr << "StreamThread: reconnect failed, retry in 3s\n";
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (running_ && std::chrono::steady_clock::now() < deadline) {
            EncodedPacket discard;
            in_queue_.pop(discard, 100);  // 持续排空队列，防止背压
        }
    }
}

void StreamThread::run() {
    using clk = std::chrono::steady_clock;
    auto last_ok = clk::now();

    while (running_) {
        EncodedPacket ep;
        if (!in_queue_.pop(ep, 200)) {
            // 5s 内没有成功写包，检查 AVIO 错误标志
            if (header_written_ && clk::now() - last_ok > std::chrono::seconds(5)) {
                if (fmt_ctx_->pb->error < 0) {
                    std::cerr << "StreamThread: connection lost (heartbeat), reconnecting...\n";
                    reconnect_loop();
                    last_ok = clk::now();
                }
            }
            continue;
        }

        if (!write_packet(ep)) {
            std::cerr << "StreamThread: write failed, reconnecting...\n";
            reconnect_loop();
            last_ok = clk::now();
        } else {
            last_ok = clk::now();
        }
    }
}
