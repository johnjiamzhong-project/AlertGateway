#pragma once
#include <thread>
#include <atomic>
#include <cstdint>
#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "common/BlockingQueue.hpp"
#include "common/Frame.hpp"
#include "common/SharedDetections.hpp"

struct EncodeConfig {
    int width;
    int height;
    int fps;
    int bitrate_kbps = 2000;
};

class EncodeThread {
public:
    EncodeThread(const EncodeConfig& cfg,
                 BlockingQueue<Frame>& in_queue,
                 BlockingQueue<EncodedPacket>& out_queue,
                 SharedDetections& shared_dets);
    ~EncodeThread();

    void start();
    void stop();

private:
    void run();
    bool init_mpp();
    void deinit_mpp();
    // Direct YUYV→NV12, no intermediate RGB buffer
    static void yuyv_to_nv12(const uint8_t* yuyv, uint8_t* nv12, int w, int h);
    // Box drawing directly on NV12 planes
    static void draw_hline_nv12(uint8_t* nv12, int w, int h, int x0, int x1, int y,
                                 uint8_t yv, uint8_t uv, uint8_t vv);
    static void draw_vline_nv12(uint8_t* nv12, int w, int h, int x, int y0, int y1,
                                 uint8_t yv, uint8_t uv, uint8_t vv);
    static void draw_rect_nv12(uint8_t* nv12, int w, int h,
                                int x1, int y1, int x2, int y2, int thickness,
                                uint8_t yv, uint8_t uv, uint8_t vv);

    EncodeConfig                  cfg_;
    BlockingQueue<Frame>&         in_queue_;
    BlockingQueue<EncodedPacket>& out_queue_;
    SharedDetections&             shared_dets_;
    std::thread                   thread_;
    std::atomic<bool>             running_{false};

    MppCtx         ctx_       = nullptr;
    MppApi*        mpi_       = nullptr;
    MppBufferGroup buf_group_ = nullptr;
    int64_t        frame_idx_ = 0;
};
