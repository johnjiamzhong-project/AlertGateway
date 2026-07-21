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
    std::string channel_id = "single";
    int width;
    int height;
    int fps;
    int bitrate_kbps = 2000;  // CBR目标码率；RTMP直播推流要稳定带宽，不用VBR换画质
    bool draw_detection_labels = true;
    int detection_alignment_delay_frames = 2; // 仅延后编码，帧率保持不变
    // 仅诊断：在真正绘制前检查显示快照中的框是否相交，不改变任何检测/跟踪结果。
    bool render_overlap_audit = true;
    float render_overlap_min_intersection_area_px = 1.0f;
};

// 编码线程：取帧 → YUYV直转NV12 → 叠最新检测框 → MPP硬编码H.264 → 写EncodedPacket队列。
// 跟 InferThread 分开是为了让编码帧率不被NPU推理速度(~40ms/帧)拖累，
// 检测框通过 shared_dets_ 这个"覆盖式共享区"读最新结果，不走队列、不等待推理完成。
class EncodeThread {
public:
    EncodeThread(const EncodeConfig& cfg,
                 BlockingQueue<Frame>& in_queue,
                 BlockingQueue<EncodedPacket>& out_queue,
                 SharedDetections& shared_dets);
    ~EncodeThread();

    void start();  // 初始化MPP编码器并启动编码线程
    void stop();   // 停线程并释放MPP资源

private:
    void run();          // 编码主循环（取帧→转格式→画框→硬编码→取包），详见 .cpp
    bool init_mpp();     // 创建MPP编码上下文、配置参数、申请DRM buffer group，详见 .cpp
    void deinit_mpp();   // 按创建的反顺序释放 buf_group_/ctx_

    // YUYV422 → NV12(YUV420SP) CPU兜底路径，仅在 RGA 硬件失败时使用，详见 .cpp
    static void yuyv_to_nv12(const uint8_t* yuyv, uint8_t* nv12, int w, int h);
    // 在NV12的Y/UV平面上直接画线/画框，没有用任何图形库，详见 .cpp
    static void draw_hline_nv12(uint8_t* nv12, int w, int h, int x0, int x1, int y,
                                 uint8_t yv, uint8_t uv, uint8_t vv);
    static void draw_vline_nv12(uint8_t* nv12, int w, int h, int x, int y0, int y1,
                                 uint8_t yv, uint8_t uv, uint8_t vv);
    static void draw_rect_nv12(uint8_t* nv12, int w, int h,
                                int x1, int y1, int x2, int y2, int thickness,
                                uint8_t yv, uint8_t uv, uint8_t vv);
    static void draw_solid_rect_nv12(uint8_t* nv12, int w, int h,
                                     int x1, int y1, int x2, int y2,
                                     uint8_t yv, uint8_t uv, uint8_t vv);
    static void draw_char_nv12(uint8_t* nv12, int w, int h,
                               int x, int y, char ch,
                               uint8_t text_y, uint8_t text_u, uint8_t text_v,
                               int scale = 1);
    static void draw_chinese_char_nv12(uint8_t* nv12, int w, int h,
                                       int x, int y, const std::string& chinese_char,
                                       uint8_t text_y, uint8_t text_u, uint8_t text_v,
                                       int scale = 1);
    static void draw_string_nv12(uint8_t* nv12, int w, int h,
                                 int x, int y, const std::string& str,
                                 uint8_t text_y, uint8_t text_u, uint8_t text_v,
                                 int scale = 1);
    static void draw_detection_label_nv12(uint8_t* nv12, int w, int h,
                                          int x1, int y1, int x2, int y2,
                                          const std::string& label_str);
    void audit_render_overlaps(const DisplayDetections& display, const Frame& frame);

    EncodeConfig                  cfg_;
    BlockingQueue<Frame>&         in_queue_;
    BlockingQueue<EncodedPacket>& out_queue_;
    SharedDetections&             shared_dets_;  // 读最新检测框用，写端是 InferThread
    std::thread                   thread_;
    std::atomic<bool>             running_{false};

    MppCtx         ctx_       = nullptr;  // MPP 编码上下文
    MppApi*        mpi_       = nullptr;  // MPP 函数接口表，编解码操作都通过它调用
    MppBufferGroup buf_group_ = nullptr;  // DRM类型buffer池，硬件编码器能直接DMA访问
    int64_t        frame_idx_ = 0;        // 累计编码帧数，用于按 cfg_.fps 推算每帧的PTS
    // 同一检测快照通常会被连续编码到多个视频帧；只审计/记录一次，避免重复刷日志。
    uint64_t       last_audited_detection_frame_id_ = UINT64_MAX;
    int64_t        last_audited_detection_pts_ms_ = INT64_MIN;
};
