#include "capture/PullStreamThread.hpp"
#include <iostream>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <vector>

#if defined(__aarch64__) || defined(__arm__)
#include <arm_neon.h>
#endif

#include "im2d.h"
#include "rga.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/mathematics.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
}

// Rockchip FFmpeg 的 AV_PIX_FMT_DRM_PRIME data[0] 以 AVDRMFrameDescriptor 开头，
// 后面紧跟 MPP buffer 引用。这里使用与 hwcontext_rkmpp.h 相同的 ABI 前缀，避免
// 交叉编译 sysroot 缺少 libdrm 的 drm_fourcc.h 时无法包含该私有扩展头。
struct RkMppDrmFrameDescriptor {
    AVDRMFrameDescriptor drm_desc;
    MppBuffer buffers[AV_DRM_MAX_PLANES];
};

static AVPixelFormat choose_nv12(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_NV12) return *format;
    }
    // Older Rockchip FFmpeg builds may expose only DRM_PRIME. Keep the fd path
    // as a compatibility fallback, but do not select it when NV12 is available:
    // four concurrent 4K DMABUF imports can exhaust the board's swiotlb pool.
    for (const AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE; ++format) {
        if (*format == AV_PIX_FMT_DRM_PRIME) return *format;
    }
    return formats[0];
}

// 格式转换辅助：将 AV_PIX_FMT_YUV420P 转换为 NV12 (YUV420SP)
static void yuv420p_to_nv12(const AVFrame* src, uint8_t* dst, int w, int h) {
    // 拷贝 Y 平面
    for (int i = 0; i < h; i++) {
        memcpy(dst + i * w, src->data[0] + i * src->linesize[0], w);
    }

    // 交错拷贝 U 和 V 到 UV 平面
    uint8_t* dst_uv = dst + w * h;
    const uint8_t* src_u = src->data[1];
    const uint8_t* src_v = src->data[2];
    int uv_h = h / 2;
    int uv_w = w / 2;
    for (int i = 0; i < uv_h; i++) {
        const uint8_t* row_u = src_u + i * src->linesize[1];
        const uint8_t* row_v = src_v + i * src->linesize[2];
        uint8_t* row_dst_uv = dst_uv + i * w;
        for (int j = 0; j < uv_w; j++) {
            row_dst_uv[2 * j]     = row_u[j]; // U
            row_dst_uv[2 * j + 1] = row_v[j]; // V
        }
    }
}

// 格式拷贝辅助：针对已是 NV12 格式的 AVFrame 直接拷贝
static void copy_nv12(const AVFrame* src, uint8_t* dst, int w, int h) {
    // Y 平面
    for (int i = 0; i < h; i++) {
        memcpy(dst + i * w, src->data[0] + i * src->linesize[0], w);
    }
    // UV 平面
    uint8_t* dst_uv = dst + w * h;
    for (int i = 0; i < h / 2; i++) {
        memcpy(dst_uv + i * w, src->data[1] + i * src->linesize[1], w);
    }
}

static void resize_nv12_nearest_planes(const uint8_t* src_y, int src_y_stride,
                                       const uint8_t* src_uv, int src_uv_stride,
                                       uint8_t* dst, int dst_stride,
                                       int sw, int sh, int dw, int dh) {
    // 4K -> 1080p is the normal production case. Avoid per-pixel integer
    // divisions and avoid first copying the complete 4K frame to another
    // temporary buffer: read every second luma/chroma sample directly.
    if (sw == dw * 2 && sh == dh * 2) {
        for (int y = 0; y < dh; ++y) {
            const uint8_t* src_row = src_y + (y * 2) * src_y_stride;
            uint8_t* dst_row = dst + y * dst_stride;
#if defined(__aarch64__) || defined(__arm__)
            int x = 0;
            for (; x + 16 <= dw; x += 16) {
                // Load 32 source luma bytes and keep every other byte.
                // This is the exact nearest-neighbor 2:1 mapping, without
                // the scalar loop's one-byte-at-a-time load overhead.
                const uint8x16x2_t samples = vld2q_u8(src_row + x * 2);
                vst1q_u8(dst_row + x, samples.val[0]);
            }
            for (; x < dw; ++x) dst_row[x] = src_row[x * 2];
#else
            for (int x = 0; x < dw; ++x) dst_row[x] = src_row[x * 2];
#endif
        }
        uint8_t* dst_uv = dst + dst_stride * dh;
        for (int y = 0; y < dh / 2; ++y) {
            const uint8_t* src_row = src_uv + (y * 2) * src_uv_stride;
            uint8_t* dst_row = dst_uv + y * dst_stride;
#if defined(__aarch64__) || defined(__arm__)
            int x = 0;
            for (; x + 32 <= dw; x += 32) {
                // Source UV pairs are selected at [0,1], [4,5], ...;
                // vld4/vst2 performs that pair-preserving decimation.
                const uint8x16x4_t samples = vld4q_u8(src_row + x * 2);
                uint8x16x2_t selected;
                selected.val[0] = samples.val[0];
                selected.val[1] = samples.val[1];
                vst2q_u8(dst_row + x, selected);
            }
            for (; x < dw; x += 2) {
                const int src_x = x * 2;
                dst_row[x] = src_row[src_x];
                dst_row[x + 1] = src_row[src_x + 1];
            }
#else
            for (int x = 0; x < dw; x += 2) {
                const int src_x = x * 2;
                dst_row[x] = src_row[src_x];
                dst_row[x + 1] = src_row[src_x + 1];
            }
#endif
        }
        return;
    }

    for (int y = 0; y < dh; ++y) {
        const int sy = std::min(sh - 1, y * sh / dh);
        for (int x = 0; x < dw; ++x) {
            const int sx = std::min(sw - 1, x * sw / dw);
            dst[y * dst_stride + x] = src_y[sy * src_y_stride + sx];
        }
    }
    uint8_t* dst_uv = dst + dst_stride * dh;
    const int src_uv_h = sh / 2;
    const int dst_uv_h = dh / 2;
    for (int y = 0; y < dst_uv_h; ++y) {
        const int sy = std::min(src_uv_h - 1, y * src_uv_h / dst_uv_h);
        for (int x = 0; x < dw; x += 2) {
            const int sx = std::min(sw - 2, (x * sw / dw) & ~1);
            dst_uv[y * dst_stride + x] = src_uv[sy * src_uv_stride + sx];
            dst_uv[y * dst_stride + x + 1] = src_uv[sy * src_uv_stride + sx + 1];
        }
    }
}

static void resize_nv12_nearest(const uint8_t* src, int src_stride,
                                uint8_t* dst, int dst_stride,
                                int sw, int sh, int dw, int dh) {
    resize_nv12_nearest_planes(src, src_stride, src + src_stride * sh, src_stride,
                               dst, dst_stride, sw, sh, dw, dh);
}

static bool rga_resize_fd(int src_fd, int src_stride, int dst_fd, int dst_stride,
                          int sw, int sh, int dw, int dh) {
    rga_buffer_t src_buf = wrapbuffer_fd(src_fd, sw, sh, RK_FORMAT_YCbCr_420_SP,
                                         src_stride, sh);
    rga_buffer_t dst_buf = wrapbuffer_fd(dst_fd, dw, dh, RK_FORMAT_YCbCr_420_SP,
                                         dst_stride, dh);
    rga_buffer_t pat_buf{};
    const IM_STATUS ret = improcess(src_buf, dst_buf, pat_buf,
                                    im_rect{0, 0, sw, sh},
                                    im_rect{0, 0, dw, dh},
                                    im_rect{0, 0, 0, 0}, 0);
    if (ret <= 0) {
        std::cerr << "[PullStream] RGA DMA-BUF resize failed: " << imStrError(ret)
                  << " (" << ret << ")\n" << std::flush;
        return false;
    }
    return true;
}

PullStreamThread::PullStreamThread(const PullStreamConfig& cfg,
                                 BlockingQueue<Frame>& enc_queue,
                                 BlockingQueue<Frame>& infer_queue,
                                 std::shared_ptr<FrameBufferPool> frame_pool)
    : cfg_(cfg), enc_queue_(enc_queue), infer_queue_(infer_queue),
      frame_pool_(std::move(frame_pool)) {}

PullStreamThread::~PullStreamThread() {
    stop();
    if (output_buffer_group_) {
        mpp_buffer_group_put(output_buffer_group_);
        output_buffer_group_ = nullptr;
    }
}

std::string PullStreamThread::log_tag() const {
    return "[channel=" + cfg_.channel_id + "] ";
}

void PullStreamThread::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&PullStreamThread::run, this);
    std::cout << log_tag() << "[PullStream] Started pulling from: " << cfg_.url << "\n" << std::flush;
}

void PullStreamThread::stop() {
    // The FFmpeg interrupt callback observes this flag during blocked network I/O.
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    std::cout << log_tag() << "[PullStream] Stopped pull stream thread.\n" << std::flush;
}

void PullStreamThread::run() {
    uint64_t next_frame_id = 0;
    AVCodecContext* codec_ctx = nullptr;
    const AVCodec* codec = nullptr;
    int video_stream_idx = -1;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    uint64_t decoded_frames = 0;
    uint64_t enc_pushed = 0;
    uint64_t enc_dropped = 0;
    uint64_t infer_pushed = 0;
    uint64_t infer_dropped = 0;
    uint64_t paced_dropped = 0;
    uint64_t read_us = 0;
    uint64_t read_calls = 0;
    uint64_t decode_send_us = 0;
    uint64_t decode_send_packets = 0;
    uint64_t decode_receive_us = 0;
    uint64_t decode_frames = 0;
    uint64_t resize_us = 0;
    uint64_t resize_frames = 0;
    uint64_t enqueue_us = 0;
    uint64_t enqueue_frames = 0;
    int mismatch_count = 0;
    int warn_count = 0;
    auto stats_t = std::chrono::steady_clock::now();

    if (cfg_.hardware_decode && !output_buffer_group_) {
        const MPP_RET group_ret = mpp_buffer_group_get_internal(
            &output_buffer_group_, MPP_BUFFER_TYPE_DRM);
        if (group_ret != MPP_OK || !output_buffer_group_) {
            std::cerr << log_tag() << "[PullStream] failed to create DRM output buffer group: "
                      << group_ret << "\n" << std::flush;
            return;
        }
    }

    while (running_) {
        // 1. 尝试打开输入流
        AVFormatContext* format_ctx = avformat_alloc_context();
        if (!format_ctx) {
            std::cerr << log_tag() << "[PullStream] avformat_alloc_context failed\n";
            break;
        }
        format_ctx->interrupt_callback.callback = interrupt_callback;
        format_ctx->interrupt_callback.opaque = this;
        AVDictionary* options = nullptr;
        // 强制 TCP 传输，防止 RTSP 丢包花屏
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        // 连接与读取超时（微秒，5秒）
        av_dict_set(&options, "stimeout", "5000000", 0);
        av_dict_set(&options, "rw_timeout", "5000000", 0);

        std::cout << log_tag() << "[PullStream] Connecting to " << cfg_.url << "...\n" << std::flush;
        int ret = avformat_open_input(&format_ctx, cfg_.url.c_str(), nullptr, &options);
        if (options) {
            av_dict_free(&options);
        }

        if (ret < 0) {
            char err_buf[256];
            av_strerror(ret, err_buf, sizeof(err_buf));
            std::cerr << log_tag() << "[PullStream] avformat_open_input failed: " << err_buf
                      << ". Retrying in " << cfg_.reconnect_sec << "s...\n" << std::flush;

            // 重试延迟退避
            for (int i = 0; i < cfg_.reconnect_sec && running_; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            continue;
        }

        // 2. 检索流信息
        ret = avformat_find_stream_info(format_ctx, nullptr);
        if (ret < 0) {
            std::cerr << "[PullStream] avformat_find_stream_info failed\n" << std::flush;
            avformat_close_input(&format_ctx);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        // 3. 寻找最佳视频流
        video_stream_idx = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (video_stream_idx < 0 || !codec) {
            std::cerr << "[PullStream] No suitable video stream found\n" << std::flush;
            avformat_close_input(&format_ctx);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        AVStream* video_stream = format_ctx->streams[video_stream_idx];

        // 4. 配置解码器上下文。板端 Rockchip FFmpeg 的 rkmpp decoder 优先输出
        // CPU 可读的 NV12；4K->1080 在本线程直接抽样，避免四路并发导入 DMA-BUF
        // 触发板端 RGA 的 swiotlb 映射耗尽。旧版本若没有 NV12，仍保留 DRM_PRIME 兼容分支。
        const AVCodec* selected_codec = codec;
        if (cfg_.hardware_decode) {
            const char* decoder_name = nullptr;
            if (format_ctx->streams[video_stream_idx]->codecpar->codec_id == AV_CODEC_ID_H264) {
                decoder_name = "h264_rkmpp";
            } else if (format_ctx->streams[video_stream_idx]->codecpar->codec_id == AV_CODEC_ID_HEVC) {
                decoder_name = "hevc_rkmpp";
            }
            if (decoder_name) {
                const AVCodec* hw_codec = avcodec_find_decoder_by_name(decoder_name);
                if (hw_codec) selected_codec = hw_codec;
                else {
                    std::cerr << log_tag() << "[PullStream] missing hardware decoder "
                              << decoder_name << "\n" << std::flush;
                    avformat_close_input(&format_ctx);
                    break;
                }
            }
        }
        codec_ctx = avcodec_alloc_context3(selected_codec);
        if (!codec_ctx) {
            std::cerr << "[PullStream] avcodec_alloc_context3 failed\n" << std::flush;
            avformat_close_input(&format_ctx);
            continue;
        }

        ret = avcodec_parameters_to_context(codec_ctx, format_ctx->streams[video_stream_idx]->codecpar);
        if (ret < 0) {
            std::cerr << "[PullStream] avcodec_parameters_to_context failed\n" << std::flush;
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&format_ctx);
            continue;
        }

        // 启用多线程解码提高 4K 效率
        codec_ctx->thread_count = cfg_.hardware_decode ? 1 : 4;
        codec_ctx->thread_type = cfg_.hardware_decode ? 0 : FF_THREAD_FRAME;
        if (cfg_.hardware_decode) {
            codec_ctx->get_format = choose_nv12;
        }

        if (cfg_.hardware_decode) {
            av_opt_set(codec_ctx->priv_data, "afbc", "off", 0);
            av_opt_set(codec_ctx->priv_data, "buf_mode", "half", 0);
        }
        ret = avcodec_open2(codec_ctx, selected_codec, nullptr);
        if (ret < 0) {
            std::cerr << "[PullStream] avcodec_open2 failed\n" << std::flush;
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&format_ctx);
            continue;
        }

        std::cout << "[PullStream] Successfully connected & opened decoder. Resolution: "
                  << codec_ctx->width << "x" << codec_ctx->height
                  << ", Format: " << av_get_pix_fmt_name(codec_ctx->pix_fmt) << "\n" << std::flush;

        double source_fps = 0.0;
        if (video_stream->avg_frame_rate.den > 0) {
            source_fps = av_q2d(video_stream->avg_frame_rate);
        } else if (video_stream->r_frame_rate.den > 0) {
            source_fps = av_q2d(video_stream->r_frame_rate);
        }
        int target_fps = cfg_.fps;
        int frame_step = 1;
        if (source_fps > 0.0 && target_fps > 0) {
            frame_step = std::max(1, (int)std::round(source_fps / target_fps));
        }
        std::cout << "[PullStream] channel=" << cfg_.channel_id
                  << " source_fps=" << (int)std::round(source_fps)
                  << " target_fps=" << target_fps
                  << " frame_step=" << frame_step << "\n" << std::flush;

        const int output_width = cfg_.output_width > 0 ? cfg_.output_width : codec_ctx->width;
        const int output_height = cfg_.output_height > 0 ? cfg_.output_height : codec_ctx->height;
        if (output_width <= 0 || output_height <= 0 || (output_width & 1) || (output_height & 1)) {
            std::cerr << log_tag() << "[PullStream] invalid decoded output size "
                      << output_width << "x" << output_height << "\n" << std::flush;
            avcodec_free_context(&codec_ctx);
            avformat_close_input(&format_ctx);
            break;
        }
        const bool resize_after_decode = output_width != codec_ctx->width ||
                                         output_height != codec_ctx->height;
        std::vector<uint8_t> decoded_nv12;
        if (resize_after_decode) {
            decoded_nv12.resize(static_cast<size_t>(codec_ctx->width) * codec_ctx->height * 3 / 2);
            std::cout << log_tag() << "[PullStream] post_decode_resize="
                      << codec_ctx->width << "x" << codec_ctx->height << " -> "
                      << output_width << "x" << output_height
                      << " (CPU NV12 path; hardware decode remains enabled)\n" << std::flush;
        }

        // 5. 解码主循环
        uint64_t frame_index = 0;
        const int64_t source_frame_duration_ms = source_fps > 0.0
            ? std::max<int64_t>(1, static_cast<int64_t>(std::llround(1000.0 / source_fps)))
            : 33;
        int64_t source_pts_origin_ms = AV_NOPTS_VALUE;
        int64_t last_media_pts_ms = AV_NOPTS_VALUE;
        while (running_) {
            const auto read_started = std::chrono::steady_clock::now();
            ret = av_read_frame(format_ctx, packet);
            read_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - read_started).count());
            ++read_calls;
            if (ret < 0) {
                // 读取完毕或出错，触发重连
                char err_buf[256];
                av_strerror(ret, err_buf, sizeof(err_buf));
                std::cerr << "[PullStream] av_read_frame read EOF or error: " << err_buf << "\n" << std::flush;
                break;
            }

            if (packet->stream_index == video_stream_idx) {
                const auto decode_send_started = std::chrono::steady_clock::now();
                ret = avcodec_send_packet(codec_ctx, packet);
                decode_send_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - decode_send_started).count());
                ++decode_send_packets;
                if (ret < 0) {
                    std::cerr << "[PullStream] avcodec_send_packet error: " << ret << "\n" << std::flush;
                    av_packet_unref(packet);
                    break;
                }

                for (;;) {
                    const auto decode_receive_started = std::chrono::steady_clock::now();
                    const int receive_ret = avcodec_receive_frame(codec_ctx, frame);
                    decode_receive_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - decode_receive_started).count());
                    if (receive_ret != 0) break;
                    ++decode_frames;
                    int w = frame->width;
                    int h = frame->height;
                    if (w != cfg_.width || h != cfg_.height || (w & 1) || (h & 1)) {
                        if (mismatch_count++ % 30 == 0) {
                            std::cerr << log_tag() << "[PullStream] Decoded resolution " << w << "x" << h
                                      << " does not match configured source " << cfg_.width
                                      << "x" << cfg_.height << "; dropping frame\n" << std::flush;
                        }
                        av_frame_unref(frame);
                        continue;
                    }
                    ++decoded_frames;

                    frame_index++;
                    if (frame_step > 1 && (frame_index % frame_step != 0)) {
                        ++paced_dropped;
                        av_frame_unref(frame);
                        continue;
                    }

                    const size_t frame_size = static_cast<size_t>(output_width) * output_height * 3 / 2;

                    Frame frame_obj;
                    frame_obj.width = output_width;
                    frame_obj.height = output_height;
                    frame_obj.frame_id = ++next_frame_id;
                    frame_obj.pixel_format = PixelFormat::NV12;
                    int64_t media_pts_ms = AV_NOPTS_VALUE;
                    if (frame->best_effort_timestamp != AV_NOPTS_VALUE &&
                        video_stream->time_base.num > 0 && video_stream->time_base.den > 0) {
                        media_pts_ms = av_rescale_q(frame->best_effort_timestamp,
                                                    video_stream->time_base,
                                                    AVRational{1, 1000});
                        if (source_pts_origin_ms == AV_NOPTS_VALUE) {
                            source_pts_origin_ms = media_pts_ms;
                        }
                        media_pts_ms -= source_pts_origin_ms;
                    }
                    const int64_t fallback_step_ms = source_frame_duration_ms * frame_step;
                    if (media_pts_ms == AV_NOPTS_VALUE ||
                        (last_media_pts_ms != AV_NOPTS_VALUE && media_pts_ms <= last_media_pts_ms)) {
                        media_pts_ms = last_media_pts_ms == AV_NOPTS_VALUE
                            ? 0
                            : last_media_pts_ms + fallback_step_ms;
                    }
                    last_media_pts_ms = media_pts_ms;
                    frame_obj.pts_ms = media_pts_ms;
                    frame_obj.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();

                    const bool drm_frame = cfg_.hardware_decode && frame->format == AV_PIX_FMT_DRM_PRIME;
                    if (drm_frame) {
                        const auto* descriptor = reinterpret_cast<const RkMppDrmFrameDescriptor*>(frame->data[0]);
                        if (!descriptor || descriptor->drm_desc.nb_objects <= 0 ||
                            descriptor->drm_desc.nb_layers <= 0 ||
                            descriptor->drm_desc.layers[0].nb_planes <= 0) {
                            std::cerr << log_tag() << "[PullStream] invalid DRM_PRIME frame descriptor\n";
                            av_frame_unref(frame);
                            continue;
                        }
                        const auto& plane = descriptor->drm_desc.layers[0].planes[0];
                        const int object_index = plane.object_index;
                        if (object_index < 0 || object_index >= descriptor->drm_desc.nb_objects) {
                            std::cerr << log_tag() << "[PullStream] invalid DRM plane object index\n";
                            av_frame_unref(frame);
                            continue;
                        }
                        const int src_fd = descriptor->drm_desc.objects[object_index].fd;
                        const int src_stride = static_cast<int>(plane.pitch);
                        MppBuffer src_buffer = descriptor->buffers[object_index];
                        if (src_fd < 0 || src_stride < w || !src_buffer) {
                            std::cerr << log_tag() << "[PullStream] incomplete DRM_PRIME NV12 descriptor fd="
                                      << src_fd << " stride=" << src_stride << "\n";
                            av_frame_unref(frame);
                            continue;
                        }

                        if (resize_after_decode) {
                            MppBuffer output_buffer = nullptr;
                            if (mpp_buffer_get(output_buffer_group_, &output_buffer, frame_size) != MPP_OK ||
                                !output_buffer) {
                                std::cerr << log_tag() << "[PullStream] DRM output buffer allocation failed\n";
                                av_frame_unref(frame);
                                continue;
                            }
                            const int dst_fd = mpp_buffer_get_fd(output_buffer);
                            const int dst_stride = output_width;
                            bool rga_ok = rga_resize_fd(src_fd, src_stride, dst_fd, dst_stride,
                                                        w, h, output_width, output_height);
                            if (!rga_ok) {
                                void* src_ptr = mpp_buffer_get_ptr(src_buffer);
                                void* dst_ptr = mpp_buffer_get_ptr(output_buffer);
                                if (src_ptr && dst_ptr) {
                                    mpp_buffer_sync_ro_begin(src_buffer);
                                    resize_nv12_nearest(static_cast<const uint8_t*>(src_ptr), src_stride,
                                                        static_cast<uint8_t*>(dst_ptr), dst_stride,
                                                        w, h, output_width, output_height);
                                    mpp_buffer_sync_ro_end(src_buffer);
                                } else {
                                    mpp_buffer_put(output_buffer);
                                    av_frame_unref(frame);
                                    continue;
                                }
                            }
                            frame_obj.dma_buffer = std::make_shared<MppBufferHolder>(output_buffer);
                            frame_obj.dma_stride = dst_stride;
                        } else {
                            mpp_buffer_inc_ref(src_buffer);
                            frame_obj.dma_buffer = std::make_shared<MppBufferHolder>(src_buffer);
                            frame_obj.dma_stride = src_stride;
                        }
                    } else if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P ||
                               frame->format == AV_PIX_FMT_NV12) {
                        const auto resize_started = std::chrono::steady_clock::now();
                        frame_obj.raw_data = frame_pool_->acquire(frame_size);
                        uint8_t* decoded_target = resize_after_decode
                            ? decoded_nv12.data() : frame_obj.raw_data.data();
                        if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
                            yuv420p_to_nv12(frame, decoded_target, w, h);
                            if (resize_after_decode) {
                                resize_nv12_nearest(decoded_target, w, frame_obj.raw_data.data(), output_width,
                                                    w, h, output_width, output_height);
                            }
                        } else if (resize_after_decode) {
                            // Hardware rkmpp normally exposes NV12 planes directly. Scale from
                            // those planes, so the 4K frame is never copied to a temporary vector.
                            resize_nv12_nearest_planes(frame->data[0], frame->linesize[0],
                                                       frame->data[1], frame->linesize[1],
                                                       frame_obj.raw_data.data(), output_width,
                                                       w, h, output_width, output_height);
                        } else {
                            copy_nv12(frame, decoded_target, w, h);
                        }
                        resize_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - resize_started).count());
                        ++resize_frames;
                    } else {
                        // 如果有其他非常规格式，打印警告并跳过（RK3588 MPP 解码或常规 RTSP 多为上述两者）
                        if (warn_count++ % 100 == 0) {
                            std::cerr << log_tag() << "[PullStream] Warning: Unsupported frame format "
                                      << frame->format << " (" << av_get_pix_fmt_name((AVPixelFormat)frame->format)
                                      << "). Skipping...\n" << std::flush;
                        }
                        continue;
                    }

                    // 与 V4L2 路径一致：编码队列满时最多等待 100ms。
                    // 拉流/解码偶发 burst 不应因容量为 1 的队列立即丢掉编码帧；
                    // 有界等待仍可在编码真正卡住时避免无限阻塞。
                    if (running_) {
                        const auto enqueue_started = std::chrono::steady_clock::now();
                        if (enc_queue_.push(frame_obj, 100)) ++enc_pushed;
                        else ++enc_dropped;
                        size_t replaced = 0;
                        if (infer_queue_.push_latest(std::move(frame_obj), &replaced)) {
                            ++infer_pushed;
                            infer_dropped += replaced;
                        } else {
                            ++infer_dropped;
                        }
                        enqueue_us += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - enqueue_started).count());
                        ++enqueue_frames;
                    }

                    auto now = std::chrono::steady_clock::now();
                    const double elapsed = std::chrono::duration<double>(now - stats_t).count();
                    if (elapsed >= 10.0) {
                        std::cout << "[PullStats] channel=" << cfg_.channel_id
                                  << " input_fps=" << (decoded_frames / elapsed)
                                  << " decoded=" << decoded_frames
                                  << " enc_push=" << enc_pushed
                                  << " enc_drop=" << enc_dropped
                                  << " infer_push=" << infer_pushed
                                  << " infer_drop=" << infer_dropped
                                  << " pace_drop=" << paced_dropped
                                  << " read_avg_ms=" << (read_calls == 0 ? 0.0 : read_us / 1000.0 / read_calls)
                                  << " decode_send_avg_ms=" << (decode_send_packets == 0 ? 0.0 : decode_send_us / 1000.0 / decode_send_packets)
                                  << " decode_receive_avg_ms=" << (decode_frames == 0 ? 0.0 : decode_receive_us / 1000.0 / decode_frames)
                                  << " resize_avg_ms=" << (resize_frames == 0 ? 0.0 : resize_us / 1000.0 / resize_frames)
                                  << " enqueue_avg_ms=" << (enqueue_frames == 0 ? 0.0 : enqueue_us / 1000.0 / enqueue_frames)
                                  << " queues=" << enc_queue_.size() << "/" << infer_queue_.size()
                                  << "\n" << std::flush;
                        decoded_frames = enc_pushed = enc_dropped = 0;
                        infer_pushed = infer_dropped = paced_dropped = 0;
                        read_us = read_calls = decode_send_us = decode_send_packets = 0;
                        decode_receive_us = decode_frames = 0;
                        resize_us = resize_frames = enqueue_us = enqueue_frames = 0;
                        stats_t = now;
                    }
                }
            }
            av_packet_unref(packet);
        }

        // 6. 连接断开清理，准备下一轮重连
        avcodec_free_context(&codec_ctx);
        avformat_close_input(&format_ctx);
        std::cout << log_tag() << "[PullStream] Connection closed. Reconnecting...\n" << std::flush;
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
}

int PullStreamThread::interrupt_callback(void* opaque) {
    const auto* self = static_cast<const PullStreamThread*>(opaque);
    return self == nullptr || !self->running_.load();
}
