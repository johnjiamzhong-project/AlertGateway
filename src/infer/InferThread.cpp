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
static std::string encode_base64(const std::vector<uint8_t>& bytes) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i=0; i<bytes.size(); i+=3) {
        uint32_t v=bytes[i]<<16;
        if(i+1<bytes.size()) v|=bytes[i+1]<<8;
        if(i+2<bytes.size()) v|=bytes[i+2];
        out += table[(v>>18)&63]; out += table[(v>>12)&63];
        out += i+1<bytes.size()?table[(v>>6)&63]:'=';
        out += i+2<bytes.size()?table[v&63]:'=';
    }
    return out;
}

// 只保存引用/配置，不做任何 RKNN 或线程相关的初始化——真正的初始化都推迟到 start()，
// 这样构造对象本身不会失败，失败（模型加载不了等）统一在 start() 里抛异常处理。
InferThread::InferThread(const ModelConfig& model_cfg,
                         const DetectionConfig& det_cfg,
                         const ImageProcessingConfig& image_cfg,
                         BlockingQueue<Frame>& in_queue,
                         BlockingQueue<std::string>& mqtt_queue,
                         SharedDetections& shared_dets)
    : cfg_(model_cfg), det_cfg_(det_cfg),
      image_cfg_(image_cfg),
      in_queue_(in_queue), mqtt_queue_(mqtt_queue), shared_dets_(shared_dets) {}

// 析构时兜底调用 stop()，保证即使调用方忘记手动 stop，线程和 RKNN 资源也不会泄漏。
InferThread::~InferThread() { stop(); }

// 由 main 在搭好 Capture/Mqtt/Encode 之后调用一次：
// 1) load_model() 加载 .rknn 并准备好 NPU 上下文，失败则直接抛异常（main 捕获后退出）；
// 2) 按model.output_layout构造双输出或Rockchip九输出后处理器；
// 3) 初始化上报时间戳、置 running_=true，再起 run() 所在的工作线程。
// 线程起来之后 start() 立即返回，不等待第一帧推理完成。
void InferThread::start() {
    YoloConfig yolo_cfg;
    yolo_cfg.conf_threshold   = cfg_.conf_threshold;
    yolo_cfg.iou_threshold    = cfg_.iou_threshold;
    yolo_cfg.target_classes   = cfg_.target_classes;
    yolo_cfg.model_input_size = 640;
    if (cfg_.output_layout == "decoded") {
        decoded_postprocess_ = std::make_unique<YoloPostprocess>(yolo_cfg);
    } else if (cfg_.output_layout == "rockchip_dfl") {
        rockchip_postprocess_ = std::make_unique<RockchipYoloPostprocess>(yolo_cfg);
    } else {
        throw std::runtime_error(
            "InferThread: unsupported model.output_layout: " + cfg_.output_layout);
    }

    if (!load_model()) throw std::runtime_error("InferThread: failed to load " + cfg_.path);

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
// (b) 查询并校验模型输出张量布局：decoded模式识别box/class，rockchip_dfl模式
//     校验三组DFL/class/score-sum共九个输出；
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

    rknn_input_output_num io_num{};
    ret = rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC || io_num.n_input != 1) {
        std::cerr << "RKNN_QUERY_IN_OUT_NUM failed or input count is not 1\n";
        return false;
    }
    n_output_ = io_num.n_output;

    rknn_tensor_attr input_attr{};
    input_attr.index = 0;
    ret = rknn_query(ctx_, RKNN_QUERY_INPUT_ATTR, &input_attr, sizeof(input_attr));
    if (ret != RKNN_SUCC || input_attr.n_dims != 4 ||
        input_attr.fmt != RKNN_TENSOR_NHWC) {
        std::cerr << "Invalid model input tensor\n";
        return false;
    }
    model_height_ = static_cast<int>(input_attr.dims[1]);
    model_width_ = static_cast<int>(input_attr.dims[2]);
    if (model_width_ != 640 || model_height_ != 640) {
        std::cerr << "Unsupported model input size: " << model_width_
                  << "x" << model_height_ << " (expected 640x640)\n";
        return false;
    }

    output_attrs_.resize(n_output_);
    for (int i = 0; i < n_output_; i++) {
        auto& attr = output_attrs_[i];
        memset(&attr, 0, sizeof(attr));
        attr.index = i;
        ret = rknn_query(ctx_, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr));
        if (ret != RKNN_SUCC) {
            std::cerr << "RKNN_QUERY_OUTPUT_ATTR failed for output " << i << "\n";
            return false;
        }
    }

    if (cfg_.output_layout == "decoded") {
        if (n_output_ != 2) {
            std::cerr << "decoded layout requires 2 outputs, got " << n_output_ << "\n";
            return false;
        }
        for (int i = 0; i < n_output_; ++i) {
            const auto& attr = output_attrs_[i];
            if (attr.n_elems == 4 * 8400) box_output_idx_ = i;
            else if (attr.n_elems == 80 * 8400) cls_output_idx_ = i;
        }
        if (box_output_idx_ < 0 || cls_output_idx_ < 0) {
            std::cerr << "decoded layout box/class outputs not found\n";
            return false;
        }
        std::cout << "Model output layout=decoded box=" << box_output_idx_
                  << " cls=" << cls_output_idx_ << "\n" << std::flush;
    } else {
        std::string validation_error;
        if (!rockchip_postprocess_->validate(
                output_attrs_, model_width_, model_height_, &validation_error)) {
            std::cerr << "Invalid rockchip_dfl outputs: " << validation_error << "\n";
            return false;
        }
        std::cout << "Model output layout=rockchip_dfl outputs=" << n_output_
                  << "\n" << std::flush;
    }

    // ── Zero-copy 输入设置 ──────────────────────────────────────────────
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

// CPU 路径：NV12 (4:2:0 Semi-Planar) → RGB888 颜色空间转换，按 BT.601 标准公式。
// 仅作为 RGA 硬件转换失败时的兜底。
static void nv12_to_rgb(const uint8_t* nv12, uint8_t* rgb, int width, int height) {
    auto clamp = [](int v) -> uint8_t {
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    const uint8_t* y_plane = nv12;
    const uint8_t* uv_plane = nv12 + width * height;
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            int y = y_plane[i * width + j];
            int uv_idx = (i / 2) * width + (j & ~1);
            int u = uv_plane[uv_idx];
            int v = uv_plane[uv_idx + 1];

            int c = y - 16;
            int d = u - 128;
            int e = v - 128;

            int rgb_idx = (i * width + j) * 3;
            rgb[rgb_idx + 0] = clamp((298 * c + 409 * e + 128) >> 8);
            rgb[rgb_idx + 1] = clamp((298 * c - 100 * d - 208 * e + 128) >> 8);
            rgb[rgb_idx + 2] = clamp((298 * c + 516 * d + 128) >> 8);
        }
    }
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
// RGA 硬件一次完成 YUV (YUYV 或 NV12) -> RGB888 + 缩放（整图拉伸，不做 letterbox）。
// 返回 false 时 dst 内容不可信，调用方应回退到 CPU 路径。
static bool rga_yuv_to_rgb_resize(const uint8_t* src, int sw, int sh, PixelFormat fmt,
                                   uint8_t* dst, int dw, int dh) {
    int rga_fmt = (fmt == PixelFormat::NV12) ? RK_FORMAT_YCbCr_420_SP : RK_FORMAT_YUYV_422;
    rga_buffer_t src_buf = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(src), sw, sh, rga_fmt);
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

struct LetterboxGeometry {
    int width;
    int height;
    int left;
    int top;
};

static LetterboxGeometry letterbox_geometry(int sw, int sh, int dw, int dh) {
    float scale = std::min(static_cast<float>(dw) / sw,
                           static_cast<float>(dh) / sh);
    int width = static_cast<int>(std::round(sw * scale));
    int height = static_cast<int>(std::round(sh * scale));
    return {width, height, (dw - width) / 2, (dh - height) / 2};
}

static void resize_rgb_letterbox(const uint8_t* src, int sw, int sh,
                                 uint8_t* dst, int dw, int dh) {
    const auto box = letterbox_geometry(sw, sh, dw, dh);
    std::fill(dst, dst + static_cast<size_t>(dw) * dh * 3, 114);
    for (int dy = 0; dy < box.height; ++dy) {
        int sy = dy * sh / box.height;
        for (int dx = 0; dx < box.width; ++dx) {
            int sx = dx * sw / box.width;
            const uint8_t* source = src + (sy * sw + sx) * 3;
            uint8_t* target = dst + ((box.top + dy) * dw + box.left + dx) * 3;
            target[0] = source[0];
            target[1] = source[1];
            target[2] = source[2];
        }
    }
}

// RGA 硬件一次完成 YUV (YUYV 或 NV12) -> RGB888 + 缩放并补 114 灰边（Letterbox）。
// 返回 false 时 dst 内容不可信，调用方应回退到 CPU 路径。
static bool rga_yuv_to_rgb_letterbox(const uint8_t* src, int sw, int sh, PixelFormat fmt,
                                     uint8_t* dst, int dw, int dh) {
    const auto box = letterbox_geometry(sw, sh, dw, dh);
    std::fill(dst, dst + static_cast<size_t>(dw) * dh * 3, 114);
    int rga_fmt = (fmt == PixelFormat::NV12) ? RK_FORMAT_YCbCr_420_SP : RK_FORMAT_YUYV_422;
    rga_buffer_t src_buf = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(src), sw, sh, rga_fmt);
    rga_buffer_t dst_buf = wrapbuffer_virtualaddr(dst, dw, dh, RK_FORMAT_RGB_888);
    rga_buffer_t pat_buf{};
    im_rect srect{0, 0, sw, sh};
    im_rect drect{box.left, box.top, box.width, box.height};
    im_rect prect{0, 0, 0, 0};
    IM_STATUS ret = improcess(src_buf, dst_buf, pat_buf, srect, drect, prect, 0);
    if (ret <= 0) {
        std::cerr << "[Infer] RGA letterbox failed: " << imStrError(ret)
                  << " (" << ret << ")\n" << std::flush;
        return false;
    }
    return true;
}

static json summarize_objects(const std::vector<Detection>& dets) {
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
    return objects;
}

// 把一帧的检测结果聚合成 MQTT 上报用的摘要：按 label 分组，统计每类出现次数和
// 该类里的最高置信度，输出形如 {"timestamp":..,"objects":[{"label","count","score"},...]}
// 的 JSON 字符串。这里只做聚合不做节流/去重，节流（report_interval_sec）和
// 去重（跟 summary_key() 比较）是 run() 调用它之后才做的事。
std::string InferThread::summarize(const std::vector<Detection>& dets, const Thumbnail* thumbnail, const std::vector<RoiEvent>* events) {
    json msg;
    msg["timestamp"] = static_cast<int64_t>(std::time(nullptr));
    msg["objects"]   = summarize_objects(dets);
    if (thumbnail && !thumbnail->data.empty()) {
        msg["thumbnail"] = {{"width", thumbnail->width}, {"height", thumbnail->height},
                             {"format", thumbnail->format}, {"data", encode_base64(thumbnail->data)}};
    }
    if (events && !events->empty()) {
        msg["roi_events"] = json::array();
        for (const auto& e : *events)
            msg["roi_events"].push_back({{"region", e.region_id}, {"label", e.label}, {"dwell_sec", e.dwell_sec}});
    }
    return msg.dump();
}

// 生成不含 timestamp 的内容签名，避免仅因上报时间变化而重复推送 MQTT。
std::string InferThread::summary_key(const std::vector<Detection>& dets) {
    return summarize_objects(dets).dump();
}

// 推理线程主循环，start() 起的线程跑的就是这个函数，running_ 为 false 时退出。
// 每轮流程：
//   1) 从 in_queue_ 阻塞取一帧（200ms超时，超时则continue重新检查running_），
//      再非阻塞排空队列里积压的旧帧，保证用的是最新一帧、不会越攒越多。
//   2) 按 infer_every_n_frames 跳帧：不是该推理的帧只往下走第4步的MQTT节流逻辑，
//      不做NPU推理（省算力，沿用上一次的 last_detections 上报）。
//   3) 真正推理的帧：YUYV422→RGB888+缩放到模型输入尺寸（优先走RGA硬件，失败回退CPU），
//      memcpy进zero-copy输入buf，rknn_run()跑NPU，按配置获取双路浮点输出或九路
//      raw INT8输出并解码成Detection列表，写入shared_dets_供EncodeThread叠框，
//      并打印各阶段耗时方便定位瓶颈。
//   4) MQTT上报：按 report_interval_sec 节流，且内容签名相同时不重复推送。
void InferThread::run() {
    //step 就是"每隔多少帧做一次真正的 NPU 推理"，用来给推理降频。
    const int step = std::max(1, cfg_.infer_every_n_frames);
    std::vector<uint8_t> rgb_buf;
    std::vector<uint8_t> input_buf(model_width_ * model_height_ * 3);
    std::vector<Detection> last_detections;
    int frame_idx = 0;
    int rga_fail_count = 0;
    RoiFilter roi_filter(image_cfg_.roi);
    TilingTask tiling_task(image_cfg_.tiling);
    ThumbnailTask thumbnail_task(image_cfg_.thumbnail);
    Thumbnail pending_thumbnail;
    bool has_thumbnail = false;
    std::vector<RoiEvent> pending_events;


    while (running_) {
        Frame frame;
        if (!in_queue_.pop(frame, 200)) continue;

        // 丢弃队列中积压的旧帧，始终对最新帧推理
        {
            Frame fresher;
            while (in_queue_.pop(fresher, 0)) frame = std::move(fresher);
        }

        if (frame_idx % step == 0) {
            has_thumbnail = false;
            pending_events.clear();
            bool done_tiling = false;
            if (image_cfg_.tiling.enabled && image_cfg_.roi.enabled) {
                auto clamp01 = [](float v) { return std::max(0.0f, std::min(1.0f, v)); };
                std::vector<RoiRect> temp_regions;
                for (const auto& r : image_cfg_.roi.regions) {
                    float x = clamp01(r.x), y = clamp01(r.y), x2 = clamp01(r.x + r.w), y2 = clamp01(r.y + r.h);
                    if (x2 > x && y2 > y) {
                        temp_regions.push_back({x * frame.width, y * frame.height, x2 * frame.width, y2 * frame.height});
                    }
                }
                if (!temp_regions.empty()) {
                    auto tiles = tiling_task.make_tiles(frame.width, frame.height, temp_regions.front());
                    if (!tiles.empty()) {
                        std::cout << "[Infer] Running ROI Tiling with " << tiles.size() << " tiles.\n" << std::flush;
                        last_detections.clear();
                        for (const auto& tile : tiles) {
                            Frame tile_frame = tiling_task.crop(frame, tile);
                            auto tile_detections = infer_once(tile_frame);
                            for (auto& d : tile_detections) {
                                d.x1 += tile.x; d.x2 += tile.x;
                                d.y1 += tile.y; d.y2 += tile.y;
                            }
                            last_detections.insert(last_detections.end(), tile_detections.begin(), tile_detections.end());
                        }
                        // Apply ROI filter to track/dwell events and filter outliers
                        last_detections = roi_filter.filter(last_detections, frame.width, frame.height, frame.timestamp_ms, &pending_events);
                        // Global NMS duplicate merge
                        last_detections = tiling_task.merge(std::move(last_detections));
                        done_tiling = true;
                    }
                }
            }

            if (!done_tiling) {
                using clk = std::chrono::steady_clock;
                auto t0 = clk::now();

                // YUYV/NV12 → RGB888 + 缩放（推理专用，编码路径已不再使用此 buf）。
                // RGA 硬件优先，失败时回退到 CPU 路径。
                bool use_letterbox = cfg_.output_layout == "rockchip_dfl";
                bool rga_ok = use_letterbox
                    ? rga_yuv_to_rgb_letterbox(frame.raw_data.data(), frame.width, frame.height, frame.pixel_format,
                                                input_buf.data(), model_width_, model_height_)
                    : rga_yuv_to_rgb_resize(frame.raw_data.data(), frame.width, frame.height, frame.pixel_format,
                                             input_buf.data(), model_width_, model_height_);
                if (!rga_ok) {
                    rgb_buf.resize(frame.width * frame.height * 3);
                    if (frame.pixel_format == PixelFormat::NV12) {
                        nv12_to_rgb(frame.raw_data.data(), rgb_buf.data(), frame.width, frame.height);
                    } else {
                        yuyv_to_rgb(frame.raw_data.data(), rgb_buf.data(), frame.width, frame.height);
                    }
                    if (use_letterbox)
                        resize_rgb_letterbox(rgb_buf.data(), frame.width, frame.height,
                                             input_buf.data(), model_width_, model_height_);
                    else
                        resize_rgb(rgb_buf.data(), frame.width, frame.height,
                                   input_buf.data(), model_width_, model_height_);
                    if (++rga_fail_count % 30 == 1) {
                        std::cerr << "[Infer] RGA path failed, using CPU fallback ("
                                  << rga_fail_count << " times)\n" << std::flush;
                    }
                }
                auto t1 = clk::now();

                // Zero-copy：直接 memcpy 进 NPU 的 DMA 输入缓冲区
                memcpy(input_mem_->virt_addr, input_buf.data(), input_buf.size());
                auto t2 = clk::now();

                int run_ret = rknn_run(ctx_, nullptr);
                auto t3 = clk::now();

                if (run_ret == RKNN_SUCC) {
                    std::vector<rknn_output> outputs(static_cast<size_t>(n_output_));
                    for (int i = 0; i < n_output_; ++i) {
                        outputs[i].index = i;
                        outputs[i].want_float = cfg_.output_layout == "decoded";
                        outputs[i].is_prealloc = 0;
                    }
                    int get_ret =
                        rknn_outputs_get(ctx_, n_output_, outputs.data(), nullptr);
                    if (get_ret == RKNN_SUCC) {
                        if (cfg_.output_layout == "decoded") {
                            constexpr int N_ANCHORS = 8400;
                            const float* box = static_cast<const float*>(
                                outputs[box_output_idx_].buf);
                            const float* cls = static_cast<const float*>(
                                outputs[cls_output_idx_].buf);
                            last_detections = decoded_postprocess_->process(
                                box, cls, N_ANCHORS, frame.width, frame.height);
                        } else {
                            last_detections = rockchip_postprocess_->process(
                                outputs, output_attrs_, model_width_, model_height_,
                                frame.width, frame.height);
                        }
                        int release_ret =
                            rknn_outputs_release(ctx_, n_output_, outputs.data());
                        if (release_ret != RKNN_SUCC)
                            std::cerr << "[Infer] rknn_outputs_release failed: "
                                      << release_ret << "\n";
                    } else {
                        std::cerr << "[Infer] rknn_outputs_get failed: "
                                  << get_ret << "\n";
                    }
                } else {
                    std::cerr << "[Infer] rknn_run failed: " << run_ret << "\n";
                }
                auto t4 = clk::now();

                auto ms = [](auto a, auto b){ return std::chrono::duration<float,std::milli>(b-a).count(); };
                std::cout << "[Infer] cpu:" << ms(t0,t1) << " cpy:" << ms(t1,t2)
                          << " npu:" << ms(t2,t3) << " cnv:" << ms(t3,t4)
                          << " total:" << ms(t0,t4) << " ms objs:" << last_detections.size() << "\n" << std::flush;

                // Apply ROI filter to filter outliers and track dwell times
                last_detections = roi_filter.filter(last_detections, frame.width, frame.height,
                                                    frame.timestamp_ms, &pending_events);
            }

            has_thumbnail = false;
            if (image_cfg_.thumbnail.enabled &&
                (!image_cfg_.thumbnail.on_detection_only || !last_detections.empty())) {
                pending_thumbnail = thumbnail_task.create(frame);
                has_thumbnail = true;
            }
            shared_dets_.set(last_detections);
        }
        frame_idx++;

        // MQTT 上报
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(
                now - last_report_time_).count() >= det_cfg_.report_interval_sec) {
            last_report_time_ = now;
            std::string key = summary_key(last_detections);
            if (key != last_summary_key_ || !pending_events.empty()) {
                last_summary_key_ = key;
                mqtt_queue_.push(summarize(last_detections, has_thumbnail ? &pending_thumbnail : nullptr, &pending_events), 100);
            }
        }
    }
}

std::vector<Detection> InferThread::infer_once(const Frame& frame) {
    std::vector<uint8_t> input(static_cast<size_t>(model_width_)*model_height_*3);
    bool letterbox = cfg_.output_layout == "rockchip_dfl";
    bool ok = letterbox
        ? rga_yuv_to_rgb_letterbox(frame.raw_data.data(), frame.width, frame.height, frame.pixel_format, input.data(), model_width_, model_height_)
        : rga_yuv_to_rgb_resize(frame.raw_data.data(), frame.width, frame.height, frame.pixel_format, input.data(), model_width_, model_height_);
    if (!ok) {
        std::vector<uint8_t> rgb_buf(frame.width * frame.height * 3);
        if (frame.pixel_format == PixelFormat::NV12) {
            nv12_to_rgb(frame.raw_data.data(), rgb_buf.data(), frame.width, frame.height);
        } else {
            yuyv_to_rgb(frame.raw_data.data(), rgb_buf.data(), frame.width, frame.height);
        }
        if (letterbox) {
            resize_rgb_letterbox(rgb_buf.data(), frame.width, frame.height, input.data(), model_width_, model_height_);
        } else {
            resize_rgb(rgb_buf.data(), frame.width, frame.height, input.data(), model_width_, model_height_);
        }
    }
    std::memcpy(input_mem_->virt_addr, input.data(), input.size());
    if (rknn_run(ctx_, nullptr) != RKNN_SUCC) return {};
    std::vector<rknn_output> outputs(static_cast<size_t>(n_output_));
    for (int i=0;i<n_output_;++i) { outputs[i].index=i; outputs[i].want_float=cfg_.output_layout=="decoded"; outputs[i].is_prealloc=0; }
    std::vector<Detection> detections;
    if (rknn_outputs_get(ctx_,n_output_,outputs.data(),nullptr)==RKNN_SUCC) {
        if (cfg_.output_layout=="decoded") {
            detections=decoded_postprocess_->process(static_cast<const float*>(outputs[box_output_idx_].buf),
                static_cast<const float*>(outputs[cls_output_idx_].buf),8400,frame.width,frame.height);
        } else detections=rockchip_postprocess_->process(outputs,output_attrs_,model_width_,model_height_,frame.width,frame.height);
        rknn_outputs_release(ctx_,n_output_,outputs.data());
    }
    return detections;
}
