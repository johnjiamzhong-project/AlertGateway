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

struct StreamConfig {
    std::string rtmp_url;
    int width;
    int height;
    int fps;
};

class StreamThread {
public:
    StreamThread(const StreamConfig& cfg, BlockingQueue<EncodedPacket>& in_queue);
    ~StreamThread();

    void start();
    void stop();

private:
    void run();
    bool open_rtmp();
    void close_rtmp();
    void reconnect_loop();
    bool write_packet(const EncodedPacket& ep);
    bool write_extradata(const uint8_t* sps, size_t sps_len,
                         const uint8_t* pps, size_t pps_len);

    struct Nalu { const uint8_t* data; size_t size; uint8_t type; };
    std::vector<Nalu> parse_annexb(const uint8_t* buf, size_t len) const;

    StreamConfig                  cfg_;
    BlockingQueue<EncodedPacket>& in_queue_;
    std::thread                   thread_;
    std::atomic<bool>             running_{false};

    AVFormatContext* fmt_ctx_   = nullptr;
    AVStream*        video_st_  = nullptr;
    bool             header_written_ = false;
    bool             extradata_set_  = false;
    std::vector<uint8_t> sps_;
    std::vector<uint8_t> pps_;
    int64_t          pkt_idx_   = 0;
};
