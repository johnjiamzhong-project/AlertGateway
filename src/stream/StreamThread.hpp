#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include "common/BlockingQueue.hpp"
#include "common/Frame.hpp"

enum class StreamSinkType {
    FixedRtmp,
    LocalFlv,
    MetricsOnly,
};

const char* stream_sink_name(StreamSinkType sink);

struct StreamConfig {
    std::string channel_id = "single";
    StreamSinkType sink = StreamSinkType::FixedRtmp;
    std::string rtmp_url;
    int width;
    int height;
    int fps;
};

// 推流线程：取 EncodedPacket(H.264裸流) → 拆NALU → 组FLV/RTMP包 → FFmpeg推流，
// 断线自动重连。是流水线最后一环，消费 EncodeThread 写的 stream_queue。
class StreamThread {
public:
    StreamThread(const StreamConfig& cfg, BlockingQueue<EncodedPacket>& in_queue);
    ~StreamThread();

    void start();  // 初始化网络库并建立首次RTMP连接，失败则抛异常
    void stop();   // 停线程并关闭RTMP连接

private:
    void run();          // 推流主循环（取包→写包→失败重连→心跳检测），详见 .cpp
    bool open_rtmp();    // 创建AVFormatContext，以flv复用格式打开RTMP连接，详见 .cpp
    void close_rtmp();   // 写trailer、关闭IO、释放AVFormatContext（sps_/pps_保留供重连用）
    void reconnect_loop();  // 断线后阻塞重试直到连上或running_=false，详见 .cpp
    bool write_packet(const EncodedPacket& ep);  // 把一个H.264包转成AVCC格式写进RTMP流，详见 .cpp
    // 把SPS/PPS打包成AVCDecoderConfigurationRecord格式塞进extradata，并触发写header
    bool write_extradata(const uint8_t* sps, size_t sps_len,
                         const uint8_t* pps, size_t pps_len);

    // 一个NALU(网络抽象层单元)在原始buf里的位置+类型；不拷贝数据，只存指针+长度
    struct Nalu { const uint8_t* data; size_t size; uint8_t type; };
    // 按Annex B格式(00 00 01 / 00 00 00 01起始码)从裸流里切出所有NALU，详见 .cpp
    std::vector<Nalu> parse_annexb(const uint8_t* buf, size_t len) const;

    StreamConfig                  cfg_;
    BlockingQueue<EncodedPacket>& in_queue_;
    std::thread                   thread_;
    std::atomic<bool>             running_{false};

    AVFormatContext* fmt_ctx_   = nullptr;  // FFmpeg 复用/IO上下文，代表这次RTMP连接
    AVStream*        video_st_  = nullptr;  // 这个连接里唯一的视频流描述符
    bool             header_written_ = false;  // 是否已经调过avformat_write_header（写包前必须先写一次）
    bool             extradata_set_  = false;  // SPS/PPS是否已经设置进extradata（只需设置一次）
    std::vector<uint8_t> sps_;  // 缓存的SPS，重连后免等下一个关键帧，直接复用
    std::vector<uint8_t> pps_;  // 缓存的PPS，同上
    int64_t          pkt_idx_   = 0;  // 兼容旧状态统计；输出 PTS 使用源时间戳
    int64_t          stream_pts_origin_ms_ = -1;
    int64_t          last_stream_pts_ms_ = -1;
};
