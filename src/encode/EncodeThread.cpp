#include "encode/EncodeThread.hpp"
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <vector>
#include <chrono>
#include "mpp_meta.h"

EncodeThread::EncodeThread(const EncodeConfig& cfg,
                           BlockingQueue<Frame>& in_queue,
                           BlockingQueue<EncodedPacket>& out_queue,
                           SharedDetections& shared_dets)
    : cfg_(cfg), in_queue_(in_queue), out_queue_(out_queue), shared_dets_(shared_dets) {}

EncodeThread::~EncodeThread() { stop(); }

void EncodeThread::start() {
    if (!init_mpp()) throw std::runtime_error("EncodeThread: failed to init MPP encoder");
    running_ = true;
    thread_ = std::thread(&EncodeThread::run, this);
}

void EncodeThread::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    deinit_mpp();
}

bool EncodeThread::init_mpp() {
    if (mpp_create(&ctx_, &mpi_) != MPP_OK) return false;
    if (mpp_init(ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) return false;

    MppEncCfg enc_cfg;
    mpp_enc_cfg_init(&enc_cfg);

    mpp_enc_cfg_set_s32(enc_cfg, "prep:width",        cfg_.width);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:height",       cfg_.height);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:hor_stride",   cfg_.width);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:ver_stride",   cfg_.height);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:format",       MPP_FMT_YUV420SP);

    mpp_enc_cfg_set_s32(enc_cfg, "codec:type",        MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(enc_cfg, "h264:profile",      77);  // Main profile — wide player support
    mpp_enc_cfg_set_s32(enc_cfg, "h264:level",        31);  // Level 3.1 — fits 640×480@30fps
    mpp_enc_cfg_set_s32(enc_cfg, "rc:mode",           MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_target",     cfg_.bitrate_kbps * 1000);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_max",        cfg_.bitrate_kbps * 1200);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_min",        cfg_.bitrate_kbps * 800);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_num",     cfg_.fps);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_denorm",  1);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_num",    cfg_.fps);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_denorm", 1);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:gop",            8);  // keyframe every ~550ms at 14.6fps

    MPP_RET ret = mpi_->control(ctx_, MPP_ENC_SET_CFG, enc_cfg);
    mpp_enc_cfg_deinit(enc_cfg);
    if (ret != MPP_OK) return false;

    mpp_buffer_group_get_internal(&buf_group_, MPP_BUFFER_TYPE_DRM);
    return true;
}

void EncodeThread::deinit_mpp() {
    if (buf_group_) { mpp_buffer_group_put(buf_group_); buf_group_ = nullptr; }
    if (ctx_)       { mpp_destroy(ctx_); ctx_ = nullptr; mpi_ = nullptr; }
}

// Direct YUYV → NV12, no RGB intermediate.
// YUYV: [Y0 U Y1 V] per pair of pixels.
// NV12 Y plane: copy Y bytes.  UV plane: copy U,V from even rows only (2× vertical subsampling).
void EncodeThread::yuyv_to_nv12(const uint8_t* yuyv, uint8_t* nv12, int w, int h) {
    uint8_t* y_plane  = nv12;
    uint8_t* uv_plane = nv12 + w * h;

    for (int row = 0; row < h; row++) {
        const uint8_t* src  = yuyv  + row * w * 2;
        uint8_t*       dy   = y_plane + row * w;
        for (int i = 0; i < w; i++)
            dy[i] = src[i * 2];  // every other byte is Y

        if ((row & 1) == 0) {
            uint8_t* duv = uv_plane + (row >> 1) * w;
            for (int i = 0; i < w; i += 2) {
                duv[i]     = src[i * 2 + 1];  // U
                duv[i + 1] = src[i * 2 + 3];  // V
            }
        }
    }
}

// NV12 box drawing.
// For green (RGB 0,255,0) in BT.601 limited range: Y=145, U=54, V=34
void EncodeThread::draw_hline_nv12(uint8_t* nv12, int w, int h, int x0, int x1, int y,
                                    uint8_t yv, uint8_t uv, uint8_t vv) {
    if (y < 0 || y >= h) return;
    x0 = std::max(x0, 0); x1 = std::min(x1, w - 1);
    uint8_t* y_plane  = nv12;
    uint8_t* uv_plane = nv12 + w * h;
    for (int x = x0; x <= x1; x++) {
        y_plane[y * w + x] = yv;
        int ux = x & ~1;
        uv_plane[(y >> 1) * w + ux]     = uv;
        uv_plane[(y >> 1) * w + ux + 1] = vv;
    }
}

void EncodeThread::draw_vline_nv12(uint8_t* nv12, int w, int h, int x, int y0, int y1,
                                    uint8_t yv, uint8_t uv, uint8_t vv) {
    if (x < 0 || x >= w) return;
    y0 = std::max(y0, 0); y1 = std::min(y1, h - 1);
    uint8_t* y_plane  = nv12;
    uint8_t* uv_plane = nv12 + w * h;
    int ux = x & ~1;
    for (int y = y0; y <= y1; y++) {
        y_plane[y * w + x] = yv;
        uv_plane[(y >> 1) * w + ux]     = uv;
        uv_plane[(y >> 1) * w + ux + 1] = vv;
    }
}

void EncodeThread::draw_rect_nv12(uint8_t* nv12, int w, int h,
                                   int x1, int y1, int x2, int y2, int thickness,
                                   uint8_t yv, uint8_t uv, uint8_t vv) {
    for (int t = 0; t < thickness; t++) {
        draw_hline_nv12(nv12, w, h, x1, x2, y1 + t, yv, uv, vv);
        draw_hline_nv12(nv12, w, h, x1, x2, y2 - t, yv, uv, vv);
        draw_vline_nv12(nv12, w, h, x1 + t, y1, y2, yv, uv, vv);
        draw_vline_nv12(nv12, w, h, x2 - t, y1, y2, yv, uv, vv);
    }
}

void EncodeThread::run() {
    const int w = cfg_.width, h = cfg_.height;
    const size_t frame_size = static_cast<size_t>(w * h * 3 / 2);
    std::vector<uint8_t> nv12_buf(frame_size);

    // Green in BT.601 YCbCr: Y=145, Cb=54, Cr=34
    constexpr uint8_t BOX_Y = 145, BOX_U = 54, BOX_V = 34;

    int    fps_count = 0;
    auto   fps_t = std::chrono::steady_clock::now();

    while (running_) {
        Frame frame;
        if (!in_queue_.pop(frame, 200)) continue;
        if (frame.raw_data.empty()) continue;

        // YUYV → NV12 directly (no RGB intermediate)
        yuyv_to_nv12(frame.raw_data.data(), nv12_buf.data(), w, h);

        // Draw latest detection boxes directly on NV12
        auto dets = shared_dets_.get();
        for (const auto& det : dets) {
            draw_rect_nv12(nv12_buf.data(), w, h,
                           static_cast<int>(det.x1), static_cast<int>(det.y1),
                           static_cast<int>(det.x2), static_cast<int>(det.y2),
                           2, BOX_Y, BOX_U, BOX_V);
        }

        // Feed NV12 to MPP
        MppBuffer frame_buf = nullptr;
        mpp_buffer_get(buf_group_, &frame_buf, frame_size);
        memcpy(mpp_buffer_get_ptr(frame_buf), nv12_buf.data(), frame_size);

        MppFrame mpp_frame = nullptr;
        mpp_frame_init(&mpp_frame);
        mpp_frame_set_width(mpp_frame,      w);
        mpp_frame_set_height(mpp_frame,     h);
        mpp_frame_set_hor_stride(mpp_frame, w);
        mpp_frame_set_ver_stride(mpp_frame, h);
        mpp_frame_set_fmt(mpp_frame,        MPP_FMT_YUV420SP);
        mpp_frame_set_pts(mpp_frame,        frame_idx_ * 1000 / cfg_.fps);
        mpp_frame_set_buffer(mpp_frame,     frame_buf);
        mpp_frame_set_eos(mpp_frame,        0);

        mpi_->encode_put_frame(ctx_, mpp_frame);
        mpp_frame_deinit(&mpp_frame);
        mpp_buffer_put(frame_buf);
        frame_idx_++;

        MppPacket packet = nullptr;
        while (mpi_->encode_get_packet(ctx_, &packet) == MPP_OK && packet) {
            EncodedPacket ep;
            ep.pts = mpp_packet_get_pts(packet);
            ep.dts = mpp_packet_get_dts(packet);

            MppMeta meta = mpp_packet_get_meta(packet);
            RK_S32 is_intra = 0;
            mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &is_intra);
            ep.is_keyframe = (is_intra != 0);

            void*  data = mpp_packet_get_pos(packet);
            size_t len  = mpp_packet_get_length(packet);
            ep.data.assign(static_cast<uint8_t*>(data),
                           static_cast<uint8_t*>(data) + len);
            mpp_packet_deinit(&packet);
            out_queue_.push(std::move(ep), 200);
            packet = nullptr;
        }

        fps_count++;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - fps_t).count() >= 10) {
            std::cerr << "Encode: " << fps_count / 10.0f << " fps\n" << std::flush;
            fps_count = 0;
            fps_t = now;
        }
    }
}
