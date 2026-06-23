#include "encode/EncodeThread.hpp"
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <vector>
#include <chrono>
#include "mpp_meta.h"

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
    if (mpp_create(&ctx_, &mpi_) != MPP_OK) return false;
    if (mpp_init(ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) return false;

    MppEncCfg enc_cfg;
    mpp_enc_cfg_init(&enc_cfg);

    mpp_enc_cfg_set_s32(enc_cfg, "prep:width",        cfg_.width);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:height",       cfg_.height);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:hor_stride",   cfg_.width);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:ver_stride",   cfg_.height);
    mpp_enc_cfg_set_s32(enc_cfg, "prep:format",       MPP_FMT_YUV420SP);  // NV12，跟 yuyv_to_nv12 输出的格式对应

    mpp_enc_cfg_set_s32(enc_cfg, "codec:type",        MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(enc_cfg, "h264:profile",      77);  // Main profile — wide player support
    mpp_enc_cfg_set_s32(enc_cfg, "h264:level",        31);  // Level 3.1 — fits 640×480@30fps
    // CBR：恒定码率，给RTMP直播推流用，带宽要稳定；不像录像场景能用VBR换更好画质
    mpp_enc_cfg_set_s32(enc_cfg, "rc:mode",           MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_target",     cfg_.bitrate_kbps * 1000);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_max",        cfg_.bitrate_kbps * 1200);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_min",        cfg_.bitrate_kbps * 800);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_num",     cfg_.fps);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_denorm",  1);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_num",    cfg_.fps);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_denorm", 1);
    // GOP小(关键帧密)抗丢包、方便随机访问但码率开销大；这里是基于低帧率+实时性优先的选择
    mpp_enc_cfg_set_s32(enc_cfg, "rc:gop",            8);  // keyframe every ~550ms at 14.6fps

    //提交配置
    MPP_RET ret = mpi_->control(ctx_, MPP_ENC_SET_CFG, enc_cfg);
    mpp_enc_cfg_deinit(enc_cfg);
    if (ret != MPP_OK) return false;

    // DRM buffer group：硬件能直接DMA访问的物理内存池，run()里每帧从这里取一块，
    // memcpy进NV12数据后交给编码器（这次memcpy是否能省掉是潜在优化方向，待确认）
    mpp_buffer_group_get_internal(&buf_group_, MPP_BUFFER_TYPE_DRM);
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

// 编码线程主循环，start() 起的线程跑的就是这个函数，running_ 为 false 时退出。
// 每轮流程：
//   1) 从 in_queue_ 阻塞取一帧（200ms超时，超时则continue重新检查running_）。
//      跟 InferThread 不同，这里不排空积压帧——enc_queue 容量只有1，本身就形成背压，
//      没有"攒帧"的空间，按顺序逐帧编码才能保证视频流畅不丢帧。
//   2) YUYV422→NV12 直转（纯CPU，无RGA加速）。
//   3) 叠框：从 shared_dets_ 读最新一次推理结果（可能是上一帧甚至更早的，不强求对齐），
//      直接在NV12像素上画绿框。
//   4) 喂给MPP硬编码器：从DRM buffer group取一块物理内存，memcpy进NV12数据，
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

    // 绿色在BT.601 YCbCr下对应：Y=145, Cb=54, Cr=34
    constexpr uint8_t BOX_Y = 145, BOX_U = 54, BOX_V = 34;

    int    fps_count = 0;
    auto   fps_t = std::chrono::steady_clock::now();

    while (running_) {
        Frame frame;
        if (!in_queue_.pop(frame, 200)) continue;
        if (frame.raw_data.empty()) continue;

        // YUYV → NV12 直转（不经过RGB中间格式）
        yuyv_to_nv12(frame.raw_data.data(), nv12_buf.data(), w, h);

        // 直接在NV12上叠最新一次的检测框
        auto dets = shared_dets_.get();
        for (const auto& det : dets) {
            draw_rect_nv12(nv12_buf.data(), w, h,
                           static_cast<int>(det.x1), static_cast<int>(det.y1),
                           static_cast<int>(det.x2), static_cast<int>(det.y2),
                           2, BOX_Y, BOX_U, BOX_V);
        }

        // 把NV12喂给MPP：从DRM buffer group取一块硬件可DMA访问的内存，
        // memcpy进去（这次拷贝理论上可省，待优化），再包装成MppFrame喂给编码器
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
        mpp_frame_set_pts(mpp_frame,        frame_idx_ * 1000 / cfg_.fps);  // 按固定帧率线性推算PTS(ms)
        mpp_frame_set_buffer(mpp_frame,     frame_buf);
        mpp_frame_set_eos(mpp_frame,        0);

        // 把这一帧真正"喂"给硬件编码器
        mpi_->encode_put_frame(ctx_, mpp_frame);
        // 销毁 MppFrame 这个"描述符"对象
        mpp_frame_deinit(&mpp_frame);
        
        mpp_buffer_put(frame_buf);
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
