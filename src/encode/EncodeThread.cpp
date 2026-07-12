#include "encode/EncodeThread.hpp"
#define FONT8x16_IMPLEMENTATION
#include "encode/font8x16.h"
#include "encode/font16x16.h"
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <vector>
#include <chrono>
#include "mpp_meta.h"
#include "im2d.h"
#include "rga.h"

static bool rga_yuyv_to_nv12(const uint8_t* src, uint8_t* dst, int w, int h) {
    rga_buffer_t src_buf = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(src), w, h, RK_FORMAT_YUYV_422);
    rga_buffer_t dst_buf = wrapbuffer_virtualaddr(dst, w, h, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t pat_buf{};

    im_rect srect{0, 0, w, h};
    im_rect drect{0, 0, w, h};
    im_rect prect{0, 0, 0, 0};

    IM_STATUS ret = improcess(src_buf, dst_buf, pat_buf, srect, drect, prect, 0);
    if (ret <= 0) {
        return false;
    }
    return true;
}

static std::string get_display_label(const std::string& label) {
    if (label == "cell phone") return "手机";
    if (label == "cup") return "杯子";
    if (label == "keyboard") return "键盘";
    if (label == "mouse") return "鼠标";
    if (label == "laptop") return "笔记本";
    if (label == "book") return "书";
    return label;
}

// 只保存引用/配置，不做MPP初始化——跟 InferThread 一样，真正的初始化推迟到 start()，
// 失败统一在 start() 里抛异常处理。
EncodeThread::EncodeThread(const EncodeConfig& cfg,
                           BlockingQueue<Frame>& in_queue,
                           BlockingQueue<EncodedPacket>& out_queue,
                           SharedDetections& shared_dets)
    : cfg_(cfg), in_queue_(in_queue), out_queue_(out_queue), shared_dets_(shared_dets) {}

// 析构兜底调用 stop()，保证线程和MPP资源不会泄漏。
EncodeThread::~EncodeThread() { stop(); }

// 由 main 调用一次：init_mpp() 配好硬编码器参数，失败则抛异常终止启动；
// 成功后置 running_=true 并起 run() 所在的工作线程，立即返回，不等待第一帧编码完成。
void EncodeThread::start() {
    if (!init_mpp()) throw std::runtime_error("EncodeThread: failed to init MPP encoder");
    running_ = true;
    thread_ = std::thread(&EncodeThread::run, this);
}

// 置 running_=false 让 run() 循环下一次检查时自行退出，join 等待线程真正结束，
// 再释放MPP资源——顺序很重要：线程没退出前不能销毁 ctx_/buf_group_。
void EncodeThread::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    deinit_mpp();
}

// 创建MPP编码上下文并一次性把所有参数（分辨率/格式/码率/GOP）通过 MppEncCfg
// 提交给硬件，不是逐个setter单独调用；最后申请DRM类型buffer group，
// 这种内存硬件编码器能直接DMA访问，跟CPU malloc的堆内存是两种东西。
bool EncodeThread::init_mpp() {
    // MPP 编码器初始化的第一步——创建上下文 + 指定工作模式
    MPP_RET ret = mpp_create(&ctx_, &mpi_);
    if (ret != MPP_OK) {
        std::cerr << "mpp_create failed: " << ret << "\n";
        return false;
    }
    ret = mpp_init(ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK) {
        std::cerr << "mpp_init encoder failed: " << ret << "\n";
        deinit_mpp();
        return false;
    }

    MppEncCfg enc_cfg;
    ret = mpp_enc_cfg_init(&enc_cfg);
    if (ret != MPP_OK) {
        std::cerr << "mpp_enc_cfg_init failed: " << ret << "\n";
        deinit_mpp();
        return false;
    }

    auto set_s32 = [&](const char* name, RK_S32 value) {
        MPP_RET set_ret = mpp_enc_cfg_set_s32(enc_cfg, name, value);
        if (set_ret != MPP_OK) {
            std::cerr << "mpp_enc_cfg_set_s32(" << name << ") failed: "
                      << set_ret << "\n";
        }
        return set_ret == MPP_OK;
    };

    bool cfg_ok = true;
    cfg_ok = set_s32("prep:width",        cfg_.width) && cfg_ok;
    cfg_ok = set_s32("prep:height",       cfg_.height) && cfg_ok;
    cfg_ok = set_s32("prep:hor_stride",   cfg_.width) && cfg_ok;
    cfg_ok = set_s32("prep:ver_stride",   cfg_.height) && cfg_ok;
    cfg_ok = set_s32("prep:format",       MPP_FMT_YUV420SP) && cfg_ok;  // NV12，跟 yuyv_to_nv12 输出的格式对应

    cfg_ok = set_s32("codec:type",        MPP_VIDEO_CodingAVC) && cfg_ok;
    cfg_ok = set_s32("h264:profile",      77) && cfg_ok;  // Main profile — wide player support
    cfg_ok = set_s32("h264:level",        31) && cfg_ok;  // Level 3.1 — fits 640×480@30fps
    // CBR：恒定码率，给RTMP直播推流用，带宽要稳定；不像录像场景能用VBR换更好画质
    cfg_ok = set_s32("rc:mode",           MPP_ENC_RC_MODE_CBR) && cfg_ok;
    cfg_ok = set_s32("rc:bps_target",     cfg_.bitrate_kbps * 1000) && cfg_ok;
    cfg_ok = set_s32("rc:bps_max",        cfg_.bitrate_kbps * 1200) && cfg_ok;
    cfg_ok = set_s32("rc:bps_min",        cfg_.bitrate_kbps * 800) && cfg_ok;
    cfg_ok = set_s32("rc:fps_in_num",     cfg_.fps) && cfg_ok;
    cfg_ok = set_s32("rc:fps_in_denorm",  1) && cfg_ok;
    cfg_ok = set_s32("rc:fps_out_num",    cfg_.fps) && cfg_ok;
    cfg_ok = set_s32("rc:fps_out_denorm", 1) && cfg_ok;
    // GOP小(关键帧密)抗丢包、方便随机访问但码率开销大；这里是基于低帧率+实时性优先的选择
    cfg_ok = set_s32("rc:gop",            8) && cfg_ok;  // keyframe every ~550ms at 14.6fps
    if (!cfg_ok) {
        mpp_enc_cfg_deinit(enc_cfg);
        deinit_mpp();
        return false;
    }

    //提交配置
    ret = mpi_->control(ctx_, MPP_ENC_SET_CFG, enc_cfg);
    mpp_enc_cfg_deinit(enc_cfg);
    if (ret != MPP_OK) {
        std::cerr << "MPP_ENC_SET_CFG failed: " << ret << "\n";
        deinit_mpp();
        return false;
    }

    // DRM buffer group：硬件能直接DMA访问的物理内存池，run()里每帧从这里取一块，
    // memcpy进NV12数据后交给编码器（这次memcpy是否能省掉是潜在优化方向，待确认）
    ret = mpp_buffer_group_get_internal(&buf_group_, MPP_BUFFER_TYPE_DRM);
    if (ret != MPP_OK || !buf_group_) {
        std::cerr << "mpp_buffer_group_get_internal(DRM) failed: "
                  << ret << "\n";
        deinit_mpp();
        return false;
    }
    return true;
}

// 按创建的反顺序释放：先还DRM buffer group，再销毁ctx_（mpi_随ctx_一起失效，置空即可）。
void EncodeThread::deinit_mpp() {
    if (buf_group_) { mpp_buffer_group_put(buf_group_); buf_group_ = nullptr; }
    if (ctx_)       { mpp_destroy(ctx_); ctx_ = nullptr; mpi_ = nullptr; }
}

// YUYV422 → NV12(YUV420SP) 直转，不经过RGB中间格式（纯CPU指针运算，没用RGA硬件，
// 这是个潜在优化点）。YUYV每2个像素打包成 [Y0 U Y1 V]，NV12是Y平面+UV平面分离存储。
// Y平面：逐像素直接取，每隔一个字节就是一个Y值。
// UV平面：YUV420SP是4:2:0，水平已经在YUYV里2像素共享一组UV，这里再只取偶数行
//        （(row&1)==0）实现垂直方向的2倍下采样，凑成4:2:0的UV平面布局。
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

// 在NV12上手写画线/画框，没有用OpenCV等图形库——直接操作Y/UV两个平面的字节。
// 颜色固定绿色：RGB(0,255,0)换算成BT.601 limited range的YCbCr是 Y=145,U=54,V=34，
// 由 run() 里的 BOX_Y/BOX_U/BOX_V 常量传进来。

// 画一条水平线：Y平面逐像素写，UV平面因为4:2:0子采样要做对齐——
// 每2×2像素共享一组UV，所以纵坐标用 y>>1 找到对应的UV行，
// 横坐标用 x & ~1 把奇数x拉回到偶数（U,V在UV平面里是相邻交替存放: U V U V...）。
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

// 画一条垂直线，原理跟 draw_hline_nv12 对称，只是循环方向换成沿y轴，
// ux 只需要算一次（x是常量），UV行号仍然要逐行用 y>>1 换算。
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

// 画矩形框：上下两条横线 + 左右两条竖线拼出矩形边框，thickness 层循环往内/外
// 各加粗一像素（上边往下叠、下边往上叠、左边往右叠、右边往左叠），不是画实心矩形。
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

void EncodeThread::draw_solid_rect_nv12(uint8_t* nv12, int w, int h,
                                         int x1, int y1, int x2, int y2,
                                         uint8_t yv, uint8_t uv, uint8_t vv) {
    x1 = std::max(0, std::min(x1, w - 1));
    x2 = std::max(0, std::min(x2, w - 1));
    y1 = std::max(0, std::min(y1, h - 1));
    y2 = std::max(0, std::min(y2, h - 1));
    if (x1 > x2 || y1 > y2) return;

    uint8_t* y_plane  = nv12;
    uint8_t* uv_plane = nv12 + w * h;

    for (int y = y1; y <= y2; ++y) {
        std::memset(y_plane + y * w + x1, yv, x2 - x1 + 1);
    }

    int uv_y1 = y1 >> 1;
    int uv_y2 = y2 >> 1;
    int uv_x1 = x1 & ~1;
    int uv_x2 = (x2 & ~1) + 1;
    if (uv_x2 >= w) uv_x2 = w - 1;

    for (int uy = uv_y1; uy <= uv_y2; ++uy) {
        uint8_t* row_ptr = uv_plane + uy * w;
        for (int ux = uv_x1; ux <= uv_x2; ux += 2) {
            row_ptr[ux]     = uv;
            row_ptr[ux + 1] = vv;
        }
    }
}

void EncodeThread::draw_char_nv12(uint8_t* nv12, int w, int h,
                                   int x, int y, char ch,
                                   uint8_t text_y, uint8_t text_u, uint8_t text_v) {
    if (ch < 0 || ch >= 128) return;

    const unsigned char* glyph = font8x16[static_cast<int>(ch)];
    uint8_t* y_plane  = nv12;
    uint8_t* uv_plane = nv12 + w * h;

    for (int r = 0; r < 16; ++r) {
        int py = y + r;
        if (py < 0 || py >= h) continue;

        unsigned char row_val = glyph[r];
        for (int c = 0; c < 8; ++c) {
            int px = x + c;
            if (px < 0 || px >= w) continue;

            if ((row_val >> (7 - c)) & 1) {
                y_plane[py * w + px] = text_y;

                int uy = py >> 1;
                int ux = px & ~1;
                uv_plane[uy * w + ux]     = text_u;
                uv_plane[uy * w + ux + 1] = text_v;
            }
        }
    }
}

void EncodeThread::draw_chinese_char_nv12(uint8_t* nv12, int w, int h,
                                           int x, int y, const std::string& chinese_char,
                                           uint8_t text_y, uint8_t text_u, uint8_t text_v) {
    const uint8_t* glyph = nullptr;
    for (const auto& item : font16x16) {
        if (item.utf8 == chinese_char) {
            glyph = &item.bitmap[0][0];
            break;
        }
    }
    if (!glyph) return;

    uint8_t* y_plane  = nv12;
    uint8_t* uv_plane = nv12 + w * h;

    for (int r = 0; r < 16; ++r) {
        int py = y + r;
        if (py < 0 || py >= h) continue;

        uint8_t b1 = glyph[r * 2];
        uint8_t b2 = glyph[r * 2 + 1];

        for (int c = 0; c < 8; ++c) {
            int px = x + c;
            if (px < 0 || px >= w) continue;

            if ((b1 >> (7 - c)) & 1) {
                y_plane[py * w + px] = text_y;
                int uy = py >> 1;
                int ux = px & ~1;
                uv_plane[uy * w + ux]     = text_u;
                uv_plane[uy * w + ux + 1] = text_v;
            }
        }
        for (int c = 0; c < 8; ++c) {
            int px = x + 8 + c;
            if (px < 0 || px >= w) continue;

            if ((b2 >> (7 - c)) & 1) {
                y_plane[py * w + px] = text_y;
                int uy = py >> 1;
                int ux = px & ~1;
                uv_plane[uy * w + ux]     = text_u;
                uv_plane[uy * w + ux + 1] = text_v;
            }
        }
    }
}

void EncodeThread::draw_string_nv12(uint8_t* nv12, int w, int h,
                                     int x, int y, const std::string& str,
                                     uint8_t text_y, uint8_t text_u, uint8_t text_v) {
    int cur_x = x;
    size_t i = 0;
    while (i < str.length()) {
        unsigned char c = str[i];
        if ((c & 0x80) == 0) {
            draw_char_nv12(nv12, w, h, cur_x, y, c, text_y, text_u, text_v);
            cur_x += 8;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < str.length()) {
                std::string chinese_char = str.substr(i, 3);
                draw_chinese_char_nv12(nv12, w, h, cur_x, y, chinese_char, text_y, text_u, text_v);
                cur_x += 16;
            }
            i += 3;
        } else if ((c & 0xF0) == 0xF0) {
            i += 4;
        } else {
            i += 1;
        }
    }
}

static int get_string_display_width(const std::string& str) {
    int w = 0;
    size_t i = 0;
    while (i < str.length()) {
        unsigned char c = str[i];
        if ((c & 0x80) == 0) {
            w += 8;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            w += 16;
            i += 3;
        } else if ((c & 0xF0) == 0xF0) {
            i += 4;
        } else {
            i += 1;
        }
    }
    return w;
}

void EncodeThread::draw_detection_label_nv12(uint8_t* nv12, int w, int h,
                                              int x1, int y1, int x2, int y2,
                                              const std::string& label_str) {
    int text_w = get_string_display_width(label_str);
    int text_h = 16;

    int pad_x = 2;
    int pad_y = 2;
    int strip_w = text_w + pad_x * 2;
    int strip_h = text_h + pad_y * 2;

    int strip_x1 = x1;
    int strip_x2 = x1 + strip_w - 1;
    int strip_y1, strip_y2;

    if (y1 - strip_h >= 0) {
        strip_y1 = y1 - strip_h;
        strip_y2 = y1 - 1;
    } else {
        strip_y1 = y1;
        strip_y2 = y1 + strip_h - 1;
    }

    if (strip_x2 >= w) {
        strip_x1 = w - strip_w;
        strip_x2 = w - 1;
    }
    if (strip_x1 < 0) {
        strip_x1 = 0;
        strip_x2 = std::min(w, strip_w) - 1;
    }

    constexpr uint8_t BG_Y = 16, BG_U = 128, BG_V = 128;
    constexpr uint8_t TXT_Y = 235, TXT_U = 128, TXT_V = 128;

    draw_solid_rect_nv12(nv12, w, h, strip_x1, strip_y1, strip_x2, strip_y2, BG_Y, BG_U, BG_V);

    int text_x = strip_x1 + pad_x;
    int text_y = strip_y1 + pad_y;
    draw_string_nv12(nv12, w, h, text_x, text_y, label_str, TXT_Y, TXT_U, TXT_V);
}

// 编码线程主循环，start() 起的线程跑的就是这个函数，running_ 为 false 时退出。
// 每轮流程：
//   1) 从 in_queue_ 阻塞取一帧（200ms超时，超时则continue重新检查running_）。
//      跟 InferThread 不同，这里不排空积压帧——enc_queue 容量只有1，本身就形成背压，
//      没有"攒帧"的空间，按顺序逐帧编码才能保证视频流畅不丢帧。
//   2) YUYV422→NV12 直转（RGA硬件优先，失败回退CPU）。
//   3) 叠框：从 shared_dets_ 读最新一次推理结果（可能是上一帧甚至更早的，不强求对齐），
//      直接在NV12像素上画绿框。
//   4) 喂给MPP硬编码器：从DRM buffer group取一块物理内存，RGA直接写入（省掉memcpy），
//      包装成MppFrame调 encode_put_frame()；PTS按 frame_idx_*1000/fps 线性递推
//      （假设严格匀速出帧，没有用实际墙钟时间戳）。
//   5) 取编码结果：encode_put_frame/encode_get_packet 是异步关系，硬件内部有流水线
//      缓冲，一次put不一定对应一次get，要用while循环把当前能取的包全部取干净，
//      否则会有包积压、时间戳错位的问题。每个包附带is_keyframe标记（从MppMeta读
//      KEY_OUTPUT_INTRA），方便下游StreamThread/RTMP判断关键帧。
void EncodeThread::run() {
    const int w = cfg_.width, h = cfg_.height;
    const size_t frame_size = static_cast<size_t>(w * h * 3 / 2);  // NV12: w*h(Y) + w*h/2(UV)
    std::vector<uint8_t> nv12_buf(frame_size);
    int rga_fail_count = 0;

    // 绿色在BT.601 YCbCr下对应：Y=145, Cb=54, Cr=34
    constexpr uint8_t BOX_Y = 145, BOX_U = 54, BOX_V = 34;

    int    fps_count = 0;
    auto   fps_t = std::chrono::steady_clock::now();

    while (running_) {
        Frame frame;
        if (!in_queue_.pop(frame, 200)) continue;
        if (frame.raw_data.empty()) continue;

        // 先从 DRM 池取 buffer，让 RGA 转换结果直接写入，省掉一次 memcpy
        MppBuffer frame_buf = nullptr;
        MPP_RET ret = mpp_buffer_get(buf_group_, &frame_buf, frame_size);
        if (ret != MPP_OK || !frame_buf) {
            std::cerr << "[Encode] mpp_buffer_get failed: " << ret << "\n";
            continue;
        }
        uint8_t* drm_ptr = static_cast<uint8_t*>(mpp_buffer_get_ptr(frame_buf));
        if (!drm_ptr) {
            std::cerr << "[Encode] mpp_buffer_get_ptr returned null\n";
            mpp_buffer_put(frame_buf);
            continue;
        }

        if (frame.pixel_format == PixelFormat::NV12) {
            // 输入已经是 NV12，直接拷贝到 MPP 的 DRM 缓冲区，完全避免格式转换
            memcpy(drm_ptr, frame.raw_data.data(), frame_size);
        } else {
            // YUYV → NV12：RGA 直接写入 DRM buffer，失败时回退 CPU + memcpy 兜底
            bool rga_ok = rga_yuyv_to_nv12(frame.raw_data.data(), drm_ptr, w, h);
            if (!rga_ok) {
                yuyv_to_nv12(frame.raw_data.data(), nv12_buf.data(), w, h);
                memcpy(drm_ptr, nv12_buf.data(), frame_size);
                if (++rga_fail_count % 30 == 1) {
                    std::cerr << "[Encode] RGA yuyv_to_nv12 failed, using CPU fallback ("
                              << rga_fail_count << " times)\n" << std::flush;
                }
            }
        }

        // 直接在 DRM buffer 上叠最新一次的检测框
        auto dets = shared_dets_.get();
        for (const auto& det : dets) {
            draw_rect_nv12(drm_ptr, w, h,
                           static_cast<int>(det.x1), static_cast<int>(det.y1),
                           static_cast<int>(det.x2), static_cast<int>(det.y2),
                           2, BOX_Y, BOX_U, BOX_V);

            if (cfg_.draw_detection_labels) {
                int confidence_pct = static_cast<int>(det.score * 100.0f);
                if (confidence_pct < 0) confidence_pct = 0;
                if (confidence_pct > 100) confidence_pct = 100;
                std::string label_str =
                    get_display_label(det.label) + " " + std::to_string(confidence_pct) + "%";

                draw_detection_label_nv12(drm_ptr, w, h,
                                          static_cast<int>(det.x1), static_cast<int>(det.y1),
                                          static_cast<int>(det.x2), static_cast<int>(det.y2),
                                          label_str);
            }
        }

        MppFrame mpp_frame = nullptr;
        ret = mpp_frame_init(&mpp_frame);
        if (ret != MPP_OK || !mpp_frame) {
            std::cerr << "[Encode] mpp_frame_init failed: " << ret << "\n";
            mpp_buffer_put(frame_buf);
            continue;
        }
        mpp_frame_set_width(mpp_frame,      w);
        mpp_frame_set_height(mpp_frame,     h);
        mpp_frame_set_hor_stride(mpp_frame, w);
        mpp_frame_set_ver_stride(mpp_frame, h);
        mpp_frame_set_fmt(mpp_frame,        MPP_FMT_YUV420SP);
        mpp_frame_set_pts(mpp_frame,        frame_idx_ * 1000 / cfg_.fps);  // 按固定帧率线性推算PTS(ms)
        mpp_frame_set_buffer(mpp_frame,     frame_buf);
        mpp_frame_set_eos(mpp_frame,        0);

        // 把这一帧真正"喂"给硬件编码器
        ret = mpi_->encode_put_frame(ctx_, mpp_frame);
        // 销毁 MppFrame 这个"描述符"对象
        mpp_frame_deinit(&mpp_frame);
        mpp_buffer_put(frame_buf);
        if (ret != MPP_OK) {
            std::cerr << "[Encode] encode_put_frame failed: " << ret << "\n";
            continue;
        }
        frame_idx_++;

        // 编码器内部有流水线缓冲，一次put不一定对应一次get，循环取干净避免包积压
        MppPacket packet = nullptr;
        while (mpi_->encode_get_packet(ctx_, &packet) == MPP_OK && packet) {
            EncodedPacket ep;
            ep.pts = mpp_packet_get_pts(packet);
            ep.dts = mpp_packet_get_dts(packet);

            // 从包的meta里读是否关键帧（I帧），下游推流/解码要靠这个标记定位GOP边界
            MppMeta meta = mpp_packet_get_meta(packet);
            RK_S32 is_intra = 0;
            ret = mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &is_intra);
            if (ret != MPP_OK) {
                std::cerr << "[Encode] mpp_meta_get_s32(KEY_OUTPUT_INTRA) failed: "
                          << ret << "\n";
            }
            ep.is_keyframe = (is_intra != 0);

            void*  data = mpp_packet_get_pos(packet);
            size_t len  = mpp_packet_get_length(packet);
            if (!data || len == 0) {
                std::cerr << "[Encode] empty encoded packet\n";
                mpp_packet_deinit(&packet);
                packet = nullptr;
                continue;
            }
            ep.data.assign(static_cast<uint8_t*>(data),
                           static_cast<uint8_t*>(data) + len);
            mpp_packet_deinit(&packet);
            out_queue_.push(std::move(ep), 200);
            packet = nullptr;
        }

        // 每10秒打印一次实际编码帧率，方便排查是否跟摄像头帧率/配置的fps一致
        fps_count++;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - fps_t).count() >= 10) {
            std::cerr << "Encode: " << fps_count / 10.0f << " fps\n" << std::flush;
            fps_count = 0;
            fps_t = now;
        }
    }
}
