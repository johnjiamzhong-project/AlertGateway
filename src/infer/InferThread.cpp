#include "infer/InferThread.hpp"
#include <fstream>
#include <stdexcept>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <map>
#include <ctime>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

InferThread::InferThread(const ModelConfig& model_cfg,
                         const DetectionConfig& det_cfg,
                         BlockingQueue<Frame>& in_queue,
                         BlockingQueue<std::string>& mqtt_queue,
                         SharedDetections& shared_dets)
    : cfg_(model_cfg), det_cfg_(det_cfg),
      in_queue_(in_queue), mqtt_queue_(mqtt_queue), shared_dets_(shared_dets) {}

InferThread::~InferThread() { stop(); }

void InferThread::start() {
    if (!load_model()) throw std::runtime_error("InferThread: failed to load " + cfg_.path);

    YoloConfig yolo_cfg;
    yolo_cfg.conf_threshold   = cfg_.conf_threshold;
    yolo_cfg.iou_threshold    = cfg_.iou_threshold;
    yolo_cfg.target_classes   = cfg_.target_classes;
    yolo_cfg.model_input_size = 640;
    postprocess_ = std::make_unique<YoloPostprocess>(yolo_cfg);

    last_report_time_ = std::chrono::steady_clock::now();
    running_ = true;
    thread_ = std::thread(&InferThread::run, this);
}

void InferThread::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    // 释放 zero-copy DMA 输入缓冲区（先于 rknn_destroy）
    if (input_mem_)  { rknn_destroy_mem(ctx_, input_mem_);  input_mem_  = nullptr; }
    if (ctx_)        { rknn_destroy(ctx_); ctx_ = 0; }
}

bool InferThread::load_model() {
    std::ifstream f(cfg_.path, std::ios::binary | std::ios::ate);
    if (!f) { std::cerr << "Cannot open model: " << cfg_.path << "\n"; return false; }
    size_t size = f.tellg();
    f.seekg(0);
    std::vector<char> model(size);
    f.read(model.data(), size);

    int ret = rknn_init(&ctx_, model.data(), size,
                         RKNN_FLAG_ENABLE_SRAM, nullptr);
    if (ret != RKNN_SUCC) { std::cerr << "rknn_init failed: " << ret << "\n"; return false; }

    std::cout << "rknn_init OK (SRAM)\n" << std::flush;

    ret = rknn_set_core_mask(ctx_, RKNN_NPU_CORE_0_1_2);
    if (ret != RKNN_SUCC) {
        std::cerr << "rknn_set_core_mask(RKNN_NPU_CORE_0_1_2) failed: " << ret << "\n";
    }

    rknn_sdk_version sdk_ver;
    if (rknn_query(ctx_, RKNN_QUERY_SDK_VERSION, &sdk_ver, sizeof(sdk_ver)) == RKNN_SUCC)
        std::cout << "RKNN API: " << sdk_ver.api_version << "  Driver: " << sdk_ver.drv_version << "\n" << std::flush;

    rknn_input_output_num io_num;
    rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    n_output_ = io_num.n_output;

    // Model has two outputs (box coords, class probabilities — split before the
    // final concat at export time so each gets its own quantization scale).
    // Identify which index is which by element count rather than assuming order.
    for (int i = 0; i < n_output_; i++) {
        rknn_tensor_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.index = i;
        rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
        if (attr.n_elems == 4 * 8400) box_output_idx_ = i;
        else if (attr.n_elems == 80 * 8400) cls_output_idx_ = i;
    }
    std::cout << "Output index: box=" << box_output_idx_ << " cls=" << cls_output_idx_ << "\n" << std::flush;

    // ── Zero-copy input setup ──────────────────────────────────────────────
    rknn_tensor_attr input_attr;
    memset(&input_attr, 0, sizeof(input_attr));
    input_attr.index = 0;
    rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &input_attr, sizeof(input_attr));

    uint32_t inp_size = input_attr.size_with_stride > 0 ? input_attr.size_with_stride : input_attr.size;
    input_mem_ = rknn_create_mem(ctx_, inp_size);
    if (!input_mem_) { std::cerr << "rknn_create_mem(input) failed\n"; return false; }

    // pass_through=0 → SDK handles format/type conversion; we send UINT8 NHWC
    input_attr.pass_through = 0;
    input_attr.type = RKNN_TENSOR_UINT8;
    input_attr.fmt  = RKNN_TENSOR_NHWC;
    ret = rknn_set_io_mem(ctx_, input_mem_, &input_attr);
    if (ret != RKNN_SUCC) { std::cerr << "rknn_set_io_mem(input) failed: " << ret << "\n"; return false; }
    std::cout << "Zero-copy input bound. size=" << inp_size << "\n" << std::flush;

    // Output: standard rknn_outputs_get(want_float=1) is used at inference time
    // (let the driver dequant), so no zero-copy output binding is needed here.

    std::cout << "Model loaded. Inputs: " << io_num.n_input
              << "  Outputs: " << n_output_ << "\n" << std::flush;
    return true;
}

static void yuyv_to_rgb(const uint8_t* yuyv, uint8_t* rgb, int width, int height) {
    auto clamp = [](int v) -> uint8_t {
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j += 2) {
            int base = (i * width + j) * 2;
            int y0 = yuyv[base],   u = yuyv[base+1];
            int y1 = yuyv[base+2], v = yuyv[base+3];
            int c0 = y0-16, c1 = y1-16, d = u-128, e = v-128;
            int out = (i * width + j) * 3;
            rgb[out+0] = clamp((298*c0 + 409*e + 128) >> 8);
            rgb[out+1] = clamp((298*c0 - 100*d - 208*e + 128) >> 8);
            rgb[out+2] = clamp((298*c0 + 516*d + 128) >> 8);
            rgb[out+3] = clamp((298*c1 + 409*e + 128) >> 8);
            rgb[out+4] = clamp((298*c1 - 100*d - 208*e + 128) >> 8);
            rgb[out+5] = clamp((298*c1 + 516*d + 128) >> 8);
        }
    }
}

static void resize_rgb(const uint8_t* src, int sw, int sh,
                        uint8_t* dst, int dw, int dh) {
    for (int dy = 0; dy < dh; dy++) {
        int sy = dy * sh / dh;
        for (int dx = 0; dx < dw; dx++) {
            int sx = dx * sw / dw;
            const uint8_t* s = src + (sy * sw + sx) * 3;
            uint8_t*       d = dst + (dy * dw + dx) * 3;
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        }
    }
}

// RGA 硬件一次完成 YUYV422 -> RGB888 + 缩放（整图拉伸，不做 letterbox）。
// 返回 false 时 dst 内容不可信，调用方应回退到 CPU 路径。
static bool rga_yuyv_to_rgb_resize(const uint8_t* src, int sw, int sh,
                                    uint8_t* dst, int dw, int dh) {
    rga_buffer_t src_buf = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(src), sw, sh, RK_FORMAT_YUYV_422);
    rga_buffer_t dst_buf = wrapbuffer_virtualaddr(dst, dw, dh, RK_FORMAT_RGB_888);
    rga_buffer_t pat_buf{};

    im_rect srect{0, 0, sw, sh};
    im_rect drect{0, 0, dw, dh};
    im_rect prect{0, 0, 0, 0};

    IM_STATUS ret = improcess(src_buf, dst_buf, pat_buf, srect, drect, prect, 0);
    if (ret <= 0) {
        std::cerr << "[Infer] RGA improcess failed: " << imStrError(ret)
                   << " (" << ret << ")\n" << std::flush;
        return false;
    }
    return true;
}

std::string InferThread::summarize(const std::vector<Detection>& dets) {
    std::map<std::string, std::pair<int, float>> tally;
    for (const auto& d : dets) {
        auto& entry = tally[d.label];
        entry.first++;
        entry.second = std::max(entry.second, d.score);
    }
    json objects = json::array();
    for (const auto& [label, cv] : tally) {
        objects.push_back({
            {"label", label},
            {"count", cv.first},
            {"score", std::round(cv.second * 100.0f) / 100.0f}
        });
    }
    json msg;
    msg["timestamp"] = static_cast<int64_t>(std::time(nullptr));
    msg["objects"]   = objects;
    return msg.dump();
}

void InferThread::run() {
    constexpr int MODEL_W = 640, MODEL_H = 640;
    const int step = std::max(1, cfg_.infer_every_n_frames);
    std::vector<uint8_t> rgb_buf;
    std::vector<uint8_t> input_buf(MODEL_W * MODEL_H * 3);
    std::vector<Detection> last_detections;
    int frame_idx = 0;
    int rga_fail_count = 0;


    while (running_) {
        Frame frame;
        if (!in_queue_.pop(frame, 200)) continue;

        // 丢弃队列中积压的旧帧，始终对最新帧推理
        {
            Frame fresher;
            while (in_queue_.pop(fresher, 0)) frame = std::move(fresher);
        }

        if (frame_idx % step == 0) {
            using clk = std::chrono::steady_clock;
            auto t0 = clk::now();

            // YUYV→RGB888 + 缩放（推理专用，编码路径已不再使用此 buf）。
            // RGA 硬件优先，失败时回退到 CPU 路径。
            bool rga_ok = rga_yuyv_to_rgb_resize(frame.raw_data.data(), frame.width, frame.height,
                                                  input_buf.data(), MODEL_W, MODEL_H);
            if (!rga_ok) {
                rgb_buf.resize(frame.width * frame.height * 3);
                yuyv_to_rgb(frame.raw_data.data(), rgb_buf.data(), frame.width, frame.height);
                resize_rgb(rgb_buf.data(), frame.width, frame.height,
                           input_buf.data(), MODEL_W, MODEL_H);
                if (++rga_fail_count % 30 == 1) {
                    std::cerr << "[Infer] RGA path failed, using CPU fallback ("
                              << rga_fail_count << " times)\n" << std::flush;
                }
            }
            auto t1 = clk::now();

            // Zero-copy: memcpy directly into NPU DMA buffer
            memcpy(input_mem_->virt_addr, input_buf.data(), MODEL_W * MODEL_H * 3);
            auto t2 = clk::now();

            rknn_run(ctx_, nullptr);
            auto t3 = clk::now();

            // Standard float output: let the RKNN driver do the internal dequant.
            // Box and class are separate outputs with independent quantization
            // scales (see BUGS.md) -- a single shared scale across box pixel
            // coords (0-640) and class probabilities (0-1) destroys class precision.
            constexpr int N_ANCHORS = 8400;
            rknn_output outputs[2];
            memset(outputs, 0, sizeof(outputs));
            outputs[box_output_idx_].want_float = 1;
            outputs[cls_output_idx_].want_float = 1;
            if (rknn_outputs_get(ctx_, n_output_, outputs, nullptr) == RKNN_SUCC) {
                const float* box = static_cast<const float*>(outputs[box_output_idx_].buf);
                const float* cls = static_cast<const float*>(outputs[cls_output_idx_].buf);
                last_detections = postprocess_->process(box, cls, N_ANCHORS,
                                                          frame.width, frame.height);
                rknn_outputs_release(ctx_, n_output_, outputs);
            }
            auto t4 = clk::now();

            auto ms = [](auto a, auto b){ return std::chrono::duration<float,std::milli>(b-a).count(); };
            std::cout << "[Infer] cpu:" << ms(t0,t1) << " cpy:" << ms(t1,t2)
                      << " npu:" << ms(t2,t3) << " cnv:" << ms(t3,t4)
                      << " total:" << ms(t0,t4) << " ms objs:" << last_detections.size() << "\n" << std::flush;

            shared_dets_.set(last_detections);
        }
        frame_idx++;

        // MQTT reporting
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_report_time_).count() >= det_cfg_.report_interval_sec) {
            last_report_time_ = now;
            std::string summary = summarize(last_detections);
            if (summary != last_summary_) {
                last_summary_ = summary;
                mqtt_queue_.push(summary, 100);
            }
        }
    }
}
