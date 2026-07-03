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

// 只保存引用/配置，不做任何 RKNN 或线程相关的初始化——真正的初始化都推迟到 start()，
// 这样构造对象本身不会失败，失败（模型加载不了等）统一在 start() 里抛异常处理。
InferThread::InferThread(const ModelConfig& model_cfg,
                         const DetectionConfig& det_cfg,
                         BlockingQueue<Frame>& in_queue,
                         BlockingQueue<std::string>& mqtt_queue,
                         SharedDetections& shared_dets)
    : cfg_(model_cfg), det_cfg_(det_cfg),
      in_queue_(in_queue), mqtt_queue_(mqtt_queue), shared_dets_(shared_dets) {}

// 析构时兜底调用 stop()，保证即使调用方忘记手动 stop，线程和 RKNN 资源也不会泄漏。
InferThread::~InferThread() { stop(); }

// 由 main 在搭好 Capture/Mqtt/Encode 之后调用一次：
// 1) load_model() 加载 .rknn 并准备好 NPU 上下文，失败则直接抛异常（main 捕获后退出）；
// 2) 用 cfg_ 里的阈值/目标类别构造 YoloPostprocess（model_input_size 固定 640，
//    要跟 load_model()/run() 里假定的模型输入尺寸保持一致）；
// 3) 初始化上报时间戳、置 running_=true，再起 run() 所在的工作线程。
// 线程起来之后 start() 立即返回，不等待第一帧推理完成。
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


// 置 running_=false 让 run() 循环在下一次检查时自行退出，join 等待线程真正结束，
// 然后才释放 RKNN 资源——顺序很重要：线程没退出前不能销毁 ctx_/input_mem_，
// 否则 run() 里还在用的 NPU 句柄会变成野指针。input_mem_ 必须先于 rknn_destroy(ctx_)
// 释放（依赖同一个 ctx_）。多次调用是安全的（ctx_/input_mem_ 置空后直接跳过)。
void InferThread::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (input_mem_)  { rknn_destroy_mem(ctx_, input_mem_);  input_mem_  = nullptr; }
    if (ctx_)        { rknn_destroy(ctx_); ctx_ = 0; }
}

// 把 cfg_.path 指向的 .rknn 文件读进内存，初始化 RKNN 运行时，并完成三件事：
// (a) 开 SRAM + 绑定三个 NPU 核心提升吞吐；
// (b) 查询模型输出张量布局，确定 box 坐标和 class 概率分别是第几个输出
//     （按元素个数区分，不能假设导出顺序，见 box_output_idx_/cls_output_idx_ 的注释）；
// (c) 预分配 zero-copy 输入用的 NPU DMA 内存并绑定，后续 run() 直接 memcpy 进去，
//     省掉 rknn_inputs_set 那次额外拷贝。
// 任意一步出错就打印日志并返回 false，调用方 start() 会因此抛异常终止启动。
bool InferThread::load_model() {
    std::ifstream f(cfg_.path, std::ios::binary | std::ios::ate);
    if (!f) { std::cerr << "Cannot open model: " << cfg_.path << "\n"; return false; }
    size_t size = f.tellg();
    f.seekg(0);
    std::vector<char> model(size);
    f.read(model.data(), size);

    // RKNN_FLAG_ENABLE_SRAM：用片上 SRAM 做权重/特征图缓存，减少 NPU 访问片外 DDR 的延迟
    int ret = rknn_init(&ctx_, model.data(), size,
                         RKNN_FLAG_ENABLE_SRAM, nullptr);
    if (ret != RKNN_SUCC) { std::cerr << "rknn_init failed: " << ret << "\n"; return false; }

    std::cout << "rknn_init OK (SRAM)\n" << std::flush;

    // 绑定全部 3 个 NPU 核心做多核并行推理（RK3588S 共3核）
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

    // 模型有两个输出（box坐标、class概率）：导出时把最后的concat拆开，
    // 让两者各自拥有独立的量化scale。这里按元素个数而不是假设固定顺序来识别哪个index是哪个。
    for (int i = 0; i < n_output_; i++) {
        rknn_tensor_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.index = i;
        rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
        if (attr.n_elems == 4 * 8400) box_output_idx_ = i;
        else if (attr.n_elems == 80 * 8400) cls_output_idx_ = i;
    }
    std::cout << "Output index: box=" << box_output_idx_ << " cls=" << cls_output_idx_ << "\n" << std::flush;

    // ── Zero-copy 输入设置 ──────────────────────────────────────────────
    rknn_tensor_attr input_attr;
    memset(&input_attr, 0, sizeof(input_attr));
    input_attr.index = 0;
    rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &input_attr, sizeof(input_attr));

    uint32_t inp_size = input_attr.size_with_stride > 0 ? input_attr.size_with_stride : input_attr.size;
    input_mem_ = rknn_create_mem(ctx_, inp_size);
    if (!input_mem_) { std::cerr << "rknn_create_mem(input) failed\n"; return false; }

    // pass_through=0 → 交给SDK内部做格式/类型转换；我们这边送的是 UINT8 NHWC 格式
    input_attr.pass_through = 0;
    input_attr.type = RKNN_TENSOR_UINT8;
    input_attr.fmt  = RKNN_TENSOR_NHWC;
    ret = rknn_set_io_mem(ctx_, input_mem_, &input_attr);
    if (ret != RKNN_SUCC) { std::cerr << "rknn_set_io_mem(input) failed: " << ret << "\n"; return false; }
    std::cout << "Zero-copy input bound. size=" << inp_size << "\n" << std::flush;

    // 输出端：推理时用标准的 rknn_outputs_get(want_float=1) 即可（交给driver内部做反量化），
    // 所以这里不需要像输入那样额外绑定 zero-copy 输出。

    std::cout << "Model loaded. Inputs: " << io_num.n_input
              << "  Outputs: " << n_output_ << "\n" << std::flush;
    return true;
}

// CPU 路径：YUYV422 → RGB888 颜色空间转换，按 BT.601 标准公式逐像素计算。
// 只在 RGA 硬件转换失败时作为兜底调用（见 rga_yuyv_to_rgb_resize 的返回值检查），
// 正常情况下不会走这里，纯CPU计算比RGA慢很多。
// YUYV422 每2个像素共享一组 U/V，所以内层循环 j 每次步进2，一次处理一对像素(y0,y1)。
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

// CPU 路径：把 RGB888 图像最近邻缩放到目标尺寸(dw×dh)，配合上面 yuyv_to_rgb 一起
// 作为 RGA 硬件失败时的兜底（RGA 是硬件一次完成转换+缩放，这里CPU分两步做）。
// 目标像素(dx,dy)按比例反算回源图坐标(sx,sy)，直接取最近的源像素，不做插值。
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

// 把一帧的检测结果聚合成 MQTT 上报用的摘要：按 label 分组，统计每类出现次数和
// 该类里的最高置信度，输出形如 {"timestamp":..,"objects":[{"label","count","score"},...]}
// 的 JSON 字符串。这里只做聚合不做节流/去重，节流（report_interval_sec）和
// 去重（跟 last_summary_ 比较）是 run() 调用它之后才做的事。
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

// 推理线程主循环，start() 起的线程跑的就是这个函数，running_ 为 false 时退出。
// 每轮流程：
//   1) 从 in_queue_ 阻塞取一帧（200ms超时，超时则continue重新检查running_），
//      再非阻塞排空队列里积压的旧帧，保证用的是最新一帧、不会越攒越多。
//   2) 按 infer_every_n_frames 跳帧：不是该推理的帧只往下走第4步的MQTT节流逻辑，
//      不做NPU推理（省算力，沿用上一次的 last_detections 上报）。
//   3) 真正推理的帧：YUYV422→RGB888+缩放到模型输入尺寸（优先走RGA硬件，失败回退CPU），
//      memcpy进zero-copy输入buf，rknn_run()跑NPU，取box/cls两路浮点输出交给
//      YoloPostprocess解码成Detection列表，写入shared_dets_供EncodeThread叠框，
//      并打印各阶段耗时方便定位瓶颈。
//   4) MQTT上报：按 report_interval_sec 节流，且内容跟 last_summary_ 相同时不重复推送。
void InferThread::run() {
    constexpr int MODEL_W = 640, MODEL_H = 640;

    //step 就是"每隔多少帧做一次真正的 NPU 推理"，用来给推理降频。
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

            // Zero-copy：直接 memcpy 进 NPU 的 DMA 输入缓冲区
            memcpy(input_mem_->virt_addr, input_buf.data(), MODEL_W * MODEL_H * 3);
            auto t2 = clk::now();

            rknn_run(ctx_, nullptr);
            auto t3 = clk::now();

            // 用标准浮点输出：交给 RKNN driver 内部做反量化。
            // box 和 cls 是各自独立量化scale的两路输出——如果共用一个scale，
            // box像素坐标(0-640)和class概率(0-1)量级差太大，会把class精度冲掉（详见 BUGS.md）。
            constexpr int N_ANCHORS = 8400;
            // 1. 准备接收结果的结构体
            rknn_output outputs[2];
            memset(outputs, 0, sizeof(outputs));
            outputs[box_output_idx_].want_float = 1;
            outputs[cls_output_idx_].want_float = 1;
            // 2. 真正去拿结果
            if (rknn_outputs_get(ctx_, n_output_, outputs, nullptr) == RKNN_SUCC) {
                // 3. 取出指针，转成float*
                const float* box = static_cast<const float*>(outputs[box_output_idx_].buf);
                const float* cls = static_cast<const float*>(outputs[cls_output_idx_].buf);
                //4. 交给后处理，再释放这块NPU内存
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

        // MQTT 上报
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
