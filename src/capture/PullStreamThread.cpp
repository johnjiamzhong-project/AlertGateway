#include "capture/PullStreamThread.hpp"
#include <iostream>
#include <chrono>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavutil/mathematics.h>
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

PullStreamThread::PullStreamThread(const PullStreamConfig& cfg,
                                 BlockingQueue<Frame>& enc_queue,
                                 BlockingQueue<Frame>& infer_queue,
                                 std::shared_ptr<FrameBufferPool> frame_pool)
    : cfg_(cfg), enc_queue_(enc_queue), infer_queue_(infer_queue),
      frame_pool_(std::move(frame_pool)) {}

PullStreamThread::~PullStreamThread() {
    stop();
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
    int mismatch_count = 0;
    int warn_count = 0;
    auto stats_t = std::chrono::steady_clock::now();

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

        // 4. 配置解码器上下文
        codec_ctx = avcodec_alloc_context3(codec);
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
        codec_ctx->thread_count = 4;
        codec_ctx->thread_type = FF_THREAD_FRAME;

        ret = avcodec_open2(codec_ctx, codec, nullptr);
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

        // 5. 解码主循环
        uint64_t frame_index = 0;
        while (running_) {
            ret = av_read_frame(format_ctx, packet);
            if (ret < 0) {
                // 读取完毕或出错，触发重连
                char err_buf[256];
                av_strerror(ret, err_buf, sizeof(err_buf));
                std::cerr << "[PullStream] av_read_frame read EOF or error: " << err_buf << "\n" << std::flush;
                break;
            }

            if (packet->stream_index == video_stream_idx) {
                ret = avcodec_send_packet(codec_ctx, packet);
                if (ret < 0) {
                    std::cerr << "[PullStream] avcodec_send_packet error: " << ret << "\n" << std::flush;
                    av_packet_unref(packet);
                    break;
                }

                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
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

                    size_t frame_size = w * h * 3 / 2; // NV12 格式占用字节数

                    Frame frame_obj;
                    frame_obj.width = w;
                    frame_obj.height = h;
                    frame_obj.frame_id = ++next_frame_id;
                    frame_obj.pixel_format = PixelFormat::NV12;
                    frame_obj.raw_data = frame_pool_->acquire(frame_size);
                    frame_obj.pts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    frame_obj.timestamp_ms = frame_obj.pts_ms;

                    // 根据 FFmpeg 吐出的帧格式做对应格式转换/拷贝
                    if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
                        yuv420p_to_nv12(frame, frame_obj.raw_data.data(), w, h);
                    } else if (frame->format == AV_PIX_FMT_NV12) {
                        copy_nv12(frame, frame_obj.raw_data.data(), w, h);
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
                        if (enc_queue_.push(frame_obj, 100)) ++enc_pushed;
                        else ++enc_dropped;
                    }

                    // 非阻塞投递推理线程（推理慢时主动丢帧）
                    if (running_) {
                        size_t replaced = 0;
                        if (infer_queue_.push_latest(std::move(frame_obj), &replaced)) {
                            ++infer_pushed;
                            infer_dropped += replaced;
                        } else {
                            ++infer_dropped;
                        }
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
                                  << " queues=" << enc_queue_.size() << "/" << infer_queue_.size()
                                  << "\n" << std::flush;
                        decoded_frames = enc_pushed = enc_dropped = 0;
                        infer_pushed = infer_dropped = paced_dropped = 0;
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
