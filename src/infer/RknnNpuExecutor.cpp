#include "infer/RknnNpuExecutor.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "im2d.h"
#include "infer/InferThread.hpp"
#include "infer/RockchipYoloPostprocess.hpp"
#include "infer/YoloPostprocess.hpp"
#include "rga.h"
#include "rknn_api.h"

namespace {

struct LetterboxGeometry {
    int width;
    int height;
    int left;
    int top;
};

LetterboxGeometry letterbox_geometry(int source_width, int source_height, int target_width, int target_height) {
    const float scale = std::min(static_cast<float>(target_width) / source_width,
                                 static_cast<float>(target_height) / source_height);
    const int width = std::max(2, static_cast<int>(std::round(source_width * scale)) & ~1);
    const int height = std::max(2, static_cast<int>(std::round(source_height * scale)) & ~1);
    return {width, height, (target_width - width) / 2, (target_height - height) / 2};
}

bool rga_to_rgb(const Frame& frame, bool letterbox, uint8_t* target, int target_width, int target_height) {
    const int format = frame.pixel_format == PixelFormat::NV12 ? RK_FORMAT_YCbCr_420_SP : RK_FORMAT_YUYV_422;
    rga_buffer_t source = frame.buffer_fd() >= 0
        ? wrapbuffer_fd(frame.buffer_fd(), frame.width, frame.height, format,
                        frame.dma_stride > 0 ? frame.dma_stride : frame.width,
                        frame.height)
        : wrapbuffer_virtualaddr(const_cast<uint8_t*>(frame.data()),
                                 frame.width, frame.height, format);
    rga_buffer_t destination = wrapbuffer_virtualaddr(target, target_width, target_height, RK_FORMAT_RGB_888);
    rga_buffer_t pattern{};
    im_rect source_rect{0, 0, frame.width, frame.height};
    im_rect destination_rect{0, 0, target_width, target_height};
    if (letterbox) {
        const auto geometry = letterbox_geometry(frame.width, frame.height, target_width, target_height);
        std::fill(target, target + static_cast<size_t>(target_width) * target_height * 3, 114);
        destination_rect = {geometry.left, geometry.top, geometry.width, geometry.height};
    }
    const int scheduler = frame.buffer_fd() >= 0 ? IM_SCHEDULER_RGA3_DEFAULT
                                                  : IM_SCHEDULER_DEFAULT;
    const IM_STATUS status = improcess(source, destination, pattern, source_rect, destination_rect,
                                       im_rect{0, 0, 0, 0}, scheduler);
    if (status <= 0) {
        std::cerr << "[NpuExecutor] RGA preprocess failed: " << imStrError(status)
                  << " (" << status << ")\n" << std::flush;
        return false;
    }
    return true;
}

uint8_t clamp_byte(int value) {
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

void yuv_to_rgb(const Frame& frame, std::vector<uint8_t>* rgb) {
    rgb->resize(static_cast<size_t>(frame.width) * frame.height * 3);
    const uint8_t* source = frame.data();
    if (frame.pixel_format == PixelFormat::NV12) {
        const uint8_t* uv = source + frame.width * frame.height;
        for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x) {
            const int c = source[y * frame.width + x] - 16;
            const int index = (y / 2) * frame.width + (x & ~1);
            const int d = uv[index] - 128, e = uv[index + 1] - 128;
            uint8_t* out = rgb->data() + (y * frame.width + x) * 3;
            out[0] = clamp_byte((298 * c + 409 * e + 128) >> 8);
            out[1] = clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8);
            out[2] = clamp_byte((298 * c + 516 * d + 128) >> 8);
        }
        return;
    }
    for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; x += 2) {
        const int index = (y * frame.width + x) * 2;
        const int y0 = source[index], u = source[index + 1], y1 = source[index + 2], v = source[index + 3];
        const int d = u - 128, e = v - 128;
        uint8_t* out = rgb->data() + (y * frame.width + x) * 3;
        for (int pixel = 0; pixel < 2; ++pixel) {
            const int c = (pixel == 0 ? y0 : y1) - 16;
            out[pixel * 3] = clamp_byte((298 * c + 409 * e + 128) >> 8);
            out[pixel * 3 + 1] = clamp_byte((298 * c - 100 * d - 208 * e + 128) >> 8);
            out[pixel * 3 + 2] = clamp_byte((298 * c + 516 * d + 128) >> 8);
        }
    }
}

void resize_rgb(const std::vector<uint8_t>& source, int source_width, int source_height,
                uint8_t* target, int target_width, int target_height, bool letterbox) {
    const auto geometry = letterbox ? letterbox_geometry(source_width, source_height, target_width, target_height)
                                    : LetterboxGeometry{target_width, target_height, 0, 0};
    if (letterbox) std::fill(target, target + static_cast<size_t>(target_width) * target_height * 3, 114);
    for (int y = 0; y < geometry.height; ++y) for (int x = 0; x < geometry.width; ++x) {
        const int source_x = x * source_width / geometry.width;
        const int source_y = y * source_height / geometry.height;
        const uint8_t* in = source.data() + (source_y * source_width + source_x) * 3;
        uint8_t* out = target + ((geometry.top + y) * target_width + geometry.left + x) * 3;
        out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
    }
}

float elapsed_ms(std::chrono::steady_clock::time_point first,
                 std::chrono::steady_clock::time_point second) {
    return std::chrono::duration<float, std::milli>(second - first).count();
}

}  // namespace

struct RknnNpuExecutor::Impl {
    explicit Impl(const ModelConfig& model)
        : path(model.path), output_layout(model.output_layout) {
        yolo.conf_threshold = model.conf_threshold;
        yolo.iou_threshold = model.iou_threshold;
        yolo.target_classes = model.target_classes;
        yolo.model_input_size = 640;
    }

    std::string path;
    std::string output_layout;
    YoloConfig yolo;
    rknn_context context = 0;
    rknn_tensor_mem* input_memory = nullptr;
    int output_count = 0;
    int box_output_index = -1;
    int class_output_index = -1;
    int model_width = 0;
    int model_height = 0;
    std::vector<rknn_tensor_attr> output_attributes;
    std::unique_ptr<YoloPostprocess> decoded_postprocess;
    std::unique_ptr<RockchipYoloPostprocess> dfl_postprocess;
    std::vector<uint8_t> cpu_rgb;
    bool started = false;
};

RknnNpuExecutor::RknnNpuExecutor(const ModelConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

RknnNpuExecutor::~RknnNpuExecutor() {
    stop();
}

bool RknnNpuExecutor::start(std::string* error) {
    if (impl_->started) return true;
    auto fail = [this, error](const std::string& message) {
        if (error) *error = message;
        std::cerr << "[NpuExecutor] " << message << "\n";
        stop();
        return false;
    };
    if (impl_->output_layout == "decoded") {
        impl_->decoded_postprocess = std::make_unique<YoloPostprocess>(impl_->yolo);
    } else if (impl_->output_layout == "rockchip_dfl") {
        impl_->dfl_postprocess = std::make_unique<RockchipYoloPostprocess>(impl_->yolo);
    } else {
        return fail("unsupported model.output_layout: " + impl_->output_layout);
    }

    std::ifstream model_file(impl_->path, std::ios::binary | std::ios::ate);
    if (!model_file) return fail("cannot open model: " + impl_->path);
    const size_t model_size = model_file.tellg();
    model_file.seekg(0);
    std::vector<char> model(model_size);
    model_file.read(model.data(), static_cast<std::streamsize>(model_size));
    if (!model_file) return fail("cannot read model: " + impl_->path);

    int ret = rknn_init(&impl_->context, model.data(), model.size(), RKNN_FLAG_ENABLE_SRAM, nullptr);
    if (ret != RKNN_SUCC) return fail("rknn_init failed: " + std::to_string(ret));
    ret = rknn_set_core_mask(impl_->context, RKNN_NPU_CORE_0_1_2);
    if (ret != RKNN_SUCC) return fail("rknn_set_core_mask(RKNN_NPU_CORE_0_1_2) failed: " + std::to_string(ret));

    rknn_input_output_num io{};
    ret = rknn_query(impl_->context, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    if (ret != RKNN_SUCC || io.n_input != 1) return fail("invalid RKNN input/output contract");
    impl_->output_count = io.n_output;
    rknn_tensor_attr input{};
    input.index = 0;
    ret = rknn_query(impl_->context, RKNN_QUERY_INPUT_ATTR, &input, sizeof(input));
    if (ret != RKNN_SUCC || input.n_dims != 4 || input.fmt != RKNN_TENSOR_NHWC) return fail("invalid RKNN input tensor");
    impl_->model_height = static_cast<int>(input.dims[1]);
    impl_->model_width = static_cast<int>(input.dims[2]);
    if (impl_->model_width != 640 || impl_->model_height != 640) return fail("unsupported model input size");

    impl_->output_attributes.resize(impl_->output_count);
    for (int index = 0; index < impl_->output_count; ++index) {
        auto& attribute = impl_->output_attributes[index];
        attribute.index = index;
        ret = rknn_query(impl_->context, RKNN_QUERY_OUTPUT_ATTR, &attribute, sizeof(attribute));
        if (ret != RKNN_SUCC) return fail("RKNN_QUERY_OUTPUT_ATTR failed: " + std::to_string(index));
        if (impl_->output_layout == "decoded") {
            if (attribute.n_elems == 4 * 8400) impl_->box_output_index = index;
            if (attribute.n_elems == 80 * 8400) impl_->class_output_index = index;
        }
    }
    if (impl_->output_layout == "decoded" && (impl_->output_count != 2 || impl_->box_output_index < 0 || impl_->class_output_index < 0)) {
        return fail("decoded model output contract mismatch");
    }
    if (impl_->output_layout == "rockchip_dfl") {
        std::string validation_error;
        if (!impl_->dfl_postprocess->validate(impl_->output_attributes, impl_->model_width, impl_->model_height, &validation_error)) {
            return fail("rockchip_dfl output contract mismatch: " + validation_error);
        }
    }

    const uint32_t bytes = input.size_with_stride > 0 ? input.size_with_stride : input.size;
    impl_->input_memory = rknn_create_mem(impl_->context, bytes);
    if (!impl_->input_memory) return fail("rknn_create_mem(input) failed");
    input.pass_through = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    ret = rknn_set_io_mem(impl_->context, impl_->input_memory, &input);
    if (ret != RKNN_SUCC) return fail("rknn_set_io_mem(input) failed: " + std::to_string(ret));
    impl_->started = true;
    std::cout << "[NpuExecutor] RKNN context ready, all-core SRAM input_bytes=" << bytes << "\n" << std::flush;
    return true;
}

void RknnNpuExecutor::stop() {
    if (!impl_) return;
    if (impl_->input_memory && impl_->context) {
        rknn_destroy_mem(impl_->context, impl_->input_memory);
        impl_->input_memory = nullptr;
    }
    if (impl_->context) {
        rknn_destroy(impl_->context);
        impl_->context = 0;
    }
    impl_->started = false;
}

bool RknnNpuExecutor::ready() const {
    return impl_ && impl_->started;
}

NpuInferenceResult RknnNpuExecutor::execute(const NpuChannelConfig& channel, const Frame& frame) {
    NpuInferenceResult result;
    result.channel_id = channel.channel_id;
    result.frame_id = frame.frame_id;
    result.frame_width = frame.width;
    result.frame_height = frame.height;
    result.pts_ms = frame.pts_ms;
    result.timestamp_ms = frame.timestamp_ms;
    result.success = false;
    if (!ready() || frame.empty() || frame.width <= 0 || frame.height <= 0) return result;

    const auto started_at = std::chrono::steady_clock::now();
    uint8_t* input = static_cast<uint8_t*>(impl_->input_memory->virt_addr);
    const bool letterbox = impl_->output_layout == "rockchip_dfl";
    if (!rga_to_rgb(frame, letterbox, input, impl_->model_width, impl_->model_height)) {
        yuv_to_rgb(frame, &impl_->cpu_rgb);
        resize_rgb(impl_->cpu_rgb, frame.width, frame.height, input,
                   impl_->model_width, impl_->model_height, letterbox);
    }
    const auto preprocessed_at = std::chrono::steady_clock::now();
    const int sync_ret = rknn_mem_sync(impl_->context, impl_->input_memory, RKNN_MEMORY_SYNC_TO_DEVICE);
    const auto synced_at = std::chrono::steady_clock::now();
    if (sync_ret != RKNN_SUCC) return result;
    const int run_ret = rknn_run(impl_->context, nullptr);
    const auto ran_at = std::chrono::steady_clock::now();
    if (run_ret != RKNN_SUCC) return result;

    std::vector<rknn_output> outputs(static_cast<size_t>(impl_->output_count));
    for (int index = 0; index < impl_->output_count; ++index) {
        outputs[index].index = index;
        outputs[index].want_float = impl_->output_layout == "decoded";
        outputs[index].is_prealloc = 0;
    }
    if (rknn_outputs_get(impl_->context, impl_->output_count, outputs.data(), nullptr) != RKNN_SUCC) return result;
    if (impl_->output_layout == "decoded") {
        result.detections = impl_->decoded_postprocess->process(
            static_cast<const float*>(outputs[impl_->box_output_index].buf),
            static_cast<const float*>(outputs[impl_->class_output_index].buf), 8400,
            frame.width, frame.height);
    } else {
        result.detections = impl_->dfl_postprocess->process(outputs, impl_->output_attributes,
            impl_->model_width, impl_->model_height, frame.width, frame.height);
    }
    const int release_ret = rknn_outputs_release(impl_->context, impl_->output_count, outputs.data());
    if (release_ret != RKNN_SUCC) return result;
    const auto completed_at = std::chrono::steady_clock::now();
    result.preprocess_ms = elapsed_ms(started_at, preprocessed_at);
    result.input_sync_ms = elapsed_ms(preprocessed_at, synced_at);
    result.npu_ms = elapsed_ms(synced_at, ran_at);
    result.output_postprocess_ms = elapsed_ms(ran_at, completed_at);
    result.success = true;
    return result;
}
