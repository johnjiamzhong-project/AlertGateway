#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <array>
#include <unordered_set>

#include "infer/RockchipYoloPostprocess.hpp"
#include "infer/YoloPostprocess.hpp"
#include "rknn_api.h"

namespace {

struct Options {
    std::string model_path;
    std::string input_path;
    std::string input_mode = "standard";
    std::string output_mode = "raw";
    std::string postprocess = "none";
    int warmup = 100;
    int runs = 1000;
    int period_ms = 0;
    bool query_perf = true;
    bool perf_detail = false;
    bool prealloc_outputs = false;
    rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO;
    std::string core_name = "auto";
    bool enable_sram = false;
    bool dump_detections = false;
    float conf_threshold = 0.25f;
    float iou_threshold = 0.45f;
    bool postprocess_stages = false;
};

struct Summary {
    double mean = 0.0;
    double median = 0.0;
    double p90 = 0.0;
    double min = 0.0;
    double max = 0.0;
};

void usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0 << " <model.rknn> [options]\n"
        << "Options:\n"
        << "  --warmup N               Warmup iterations (default: 100)\n"
        << "  --runs N                 Measured iterations (default: 1000)\n"
        << "  --period-ms N            Fixed start-to-start period; 0 runs continuously\n"
        << "  --no-perf-query          Do not call RKNN_QUERY_PERF_RUN after each run\n"
        << "  --perf-detail            Collect and print one RKNN per-operator performance report\n"
        << "  --core auto|0|1|2|01|012|all\n"
        << "  --sram                   Enable RKNN_FLAG_ENABLE_SRAM\n"
        << "  --input-mode MODE        standard or zero-copy (default: standard)\n"
        << "  --output-mode MODE       raw or float (default: raw)\n"
        << "  --prealloc-outputs       Reuse caller-owned raw output buffers\n"
        << "  --postprocess MODE       none, current, or rockchip (default: none)\n"
        << "  --conf-threshold FLOAT   Detection threshold (default: 0.25)\n"
        << "  --iou-threshold FLOAT    NMS IoU threshold (default: 0.45)\n"
        << "  --postprocess-stages     Measure and print four stages of Rockchip postprocess\n"
        << "  --dump-detections        Print detections from the final iteration\n"
        << "  --input FILE             Raw UINT8 NHWC input; size must match tensor\n";
}

float parse_probability(const std::string& value, const char* option) {
    size_t consumed = 0;
    float parsed = 0.0f;
    try {
        parsed = std::stof(value, &consumed);
    } catch (...) {
        throw std::runtime_error(std::string(option) + " requires a number");
    }
    if (consumed != value.size() || parsed <= 0.0f || parsed >= 1.0f)
        throw std::runtime_error(std::string(option) + " must be between 0 and 1");
    return parsed;
}

int parse_positive(const std::string& value, const char* option) {
    size_t consumed = 0;
    int parsed = 0;
    try {
        parsed = std::stoi(value, &consumed);
    } catch (...) {
        throw std::runtime_error(std::string(option) + " requires an integer");
    }
    if (consumed != value.size() || parsed <= 0)
        throw std::runtime_error(std::string(option) + " requires a positive integer");
    return parsed;
}

int parse_nonnegative(const std::string& value, const char* option) {
    size_t consumed = 0;
    int parsed = 0;
    try {
        parsed = std::stoi(value, &consumed);
    } catch (...) {
        throw std::runtime_error(std::string(option) + " requires an integer");
    }
    if (consumed != value.size() || parsed < 0)
        throw std::runtime_error(std::string(option) + " requires a non-negative integer");
    return parsed;
}

void parse_core(const std::string& value, Options& opts) {
    opts.core_name = value;
    if (value == "auto") opts.core_mask = RKNN_NPU_CORE_AUTO;
    else if (value == "0") opts.core_mask = RKNN_NPU_CORE_0;
    else if (value == "1") opts.core_mask = RKNN_NPU_CORE_1;
    else if (value == "2") opts.core_mask = RKNN_NPU_CORE_2;
    else if (value == "01") opts.core_mask = RKNN_NPU_CORE_0_1;
    else if (value == "012") opts.core_mask = RKNN_NPU_CORE_0_1_2;
    else if (value == "all") opts.core_mask = RKNN_NPU_CORE_ALL;
    else throw std::runtime_error("unsupported --core value: " + value);
}

Options parse_args(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        throw std::runtime_error("model path is required");
    }

    Options opts;
    opts.model_path = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        auto require_value = [&](const char* option) -> std::string {
            if (++i >= argc) throw std::runtime_error(std::string(option) + " requires a value");
            return argv[i];
        };

        if (arg == "--warmup") opts.warmup = parse_positive(require_value("--warmup"), "--warmup");
        else if (arg == "--runs") opts.runs = parse_positive(require_value("--runs"), "--runs");
        else if (arg == "--period-ms")
            opts.period_ms = parse_nonnegative(require_value("--period-ms"), "--period-ms");
        else if (arg == "--core") parse_core(require_value("--core"), opts);
        else if (arg == "--input-mode") {
            opts.input_mode = require_value("--input-mode");
            if (opts.input_mode != "standard" && opts.input_mode != "zero-copy")
                throw std::runtime_error("--input-mode must be standard or zero-copy");
        }
        else if (arg == "--output-mode") {
            opts.output_mode = require_value("--output-mode");
            if (opts.output_mode != "raw" && opts.output_mode != "float")
                throw std::runtime_error("--output-mode must be raw or float");
        }
        else if (arg == "--postprocess") {
            opts.postprocess = require_value("--postprocess");
            if (opts.postprocess != "none" && opts.postprocess != "current" &&
                opts.postprocess != "rockchip")
                throw std::runtime_error(
                    "--postprocess must be none, current, or rockchip");
        }
        else if (arg == "--conf-threshold")
            opts.conf_threshold =
                parse_probability(require_value("--conf-threshold"), "--conf-threshold");
        else if (arg == "--iou-threshold")
            opts.iou_threshold =
                parse_probability(require_value("--iou-threshold"), "--iou-threshold");
        else if (arg == "--input") opts.input_path = require_value("--input");
        else if (arg == "--sram") opts.enable_sram = true;
        else if (arg == "--dump-detections") opts.dump_detections = true;
        else if (arg == "--prealloc-outputs") opts.prealloc_outputs = true;
        else if (arg == "--no-perf-query") opts.query_perf = false;
        else if (arg == "--perf-detail") opts.perf_detail = true;
        else if (arg == "--postprocess-stages") opts.postprocess_stages = true;
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown option: " + arg);
        }
    }
    return opts;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open: " + path);
    std::streamsize size = file.tellg();
    if (size <= 0) throw std::runtime_error("empty file: " + path);
    file.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size))
        throw std::runtime_error("failed to read: " + path);
    return data;
}

Summary summarize(std::vector<double> values) {
    if (values.empty()) return {};
    std::sort(values.begin(), values.end());
    Summary result;
    result.mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    size_t middle = values.size() / 2;
    result.median = values.size() % 2
        ? values[middle]
        : (values[middle - 1] + values[middle]) / 2.0;
    size_t p90_index = static_cast<size_t>(std::ceil(values.size() * 0.90)) - 1;
    result.p90 = values[std::min(p90_index, values.size() - 1)];
    result.min = values.front();
    result.max = values.back();
    return result;
}

void print_summary(const char* name, const Summary& s) {
    std::cout << std::left << std::setw(18) << name
              << " mean=" << std::fixed << std::setprecision(3) << s.mean
              << " median=" << s.median
              << " p90=" << s.p90
              << " min=" << s.min
              << " max=" << s.max << " ms\n";
}

void print_tensor(const char* kind, uint32_t index, const rknn_tensor_attr& attr) {
    std::cout << kind << '[' << index << "] name=" << attr.name << " dims=";
    for (uint32_t i = 0; i < attr.n_dims; ++i) {
        if (i) std::cout << 'x';
        std::cout << attr.dims[i];
    }
    std::cout << " fmt=" << get_format_string(attr.fmt)
              << " type=" << get_type_string(attr.type)
              << " qnt=" << get_qnt_type_string(attr.qnt_type)
              << " zp=" << attr.zp
              << " scale=" << attr.scale
              << " elems=" << attr.n_elems
              << " size=" << attr.size << '\n';
}

double elapsed_ms(std::chrono::steady_clock::time_point start,
                  std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void check_rknn(int ret, const char* operation) {
    if (ret != RKNN_SUCC)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(ret));
}

void print_detections(const std::vector<Detection>& detections) {
    std::cout << "--- detections count=" << detections.size() << " ---\n";
    for (size_t i = 0; i < detections.size(); ++i) {
        const auto& det = detections[i];
        std::cout << "det[" << i << "] class=" << det.class_id
                  << " label=" << det.label
                  << " score=" << std::fixed << std::setprecision(4) << det.score
                  << " box=(" << det.x1 << ',' << det.y1 << ','
                  << det.x2 << ',' << det.y2 << ")\n";
    }
}

constexpr int kCocoClasses = 80;
constexpr int kDesktopClasses = 6;
constexpr const char* kDesktopLabels[kDesktopClasses] = {
    "cell phone", "cup", "keyboard", "mouse", "laptop", "book"
};
constexpr int kBranches = 3;
constexpr int kOutputsPerBranch = 3;
constexpr int kMaxDflValues = 64;

float dequantize(int8_t value, const rknn_tensor_attr& attr) {
    return (static_cast<int32_t>(value) - attr.zp) * attr.scale;
}

int8_t quantize_i8(float value, const rknn_tensor_attr& attr) {
    float quantized = value / attr.scale + attr.zp;
    quantized = std::max(-128.0f, std::min(127.0f, quantized));
    return static_cast<int8_t>(quantized);
}

void compute_dfl(const float* logits, int dfl_len, float distances[4]) {
    for (int side = 0; side < 4; ++side) {
        const float* side_logits = logits + side * dfl_len;
        float max_logit = *std::max_element(side_logits, side_logits + dfl_len);
        float exp_sum = 0.0f;
        float weighted_sum = 0.0f;
        for (int i = 0; i < dfl_len; ++i) {
            float value = std::exp(side_logits[i] - max_logit);
            exp_sum += value;
            weighted_sum += value * i;
        }
        distances[side] = weighted_sum / exp_sum;
    }
}

float iou(const Detection& a, const Detection& b) {
    float x1 = std::max(a.x1, b.x1);
    float y1 = std::max(a.y1, b.y1);
    float x2 = std::min(a.x2, b.x2);
    float y2 = std::min(a.y2, b.y2);
    float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    if (intersection <= 0.0f) return 0.0f;
    float area_a = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
    float area_b = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
    return intersection / (area_a + area_b - intersection + 1e-6f);
}

std::vector<Detection> run_nms(std::vector<Detection> candidates, float iou_threshold) {
    std::sort(candidates.begin(), candidates.end(),
              [](const Detection& a, const Detection& b) {
                  return a.score > b.score;
              });
    std::vector<bool> suppressed(candidates.size(), false);
    std::vector<Detection> result;
    result.reserve(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i]) continue;
        result.push_back(candidates[i]);
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (!suppressed[j] &&
                candidates[i].class_id == candidates[j].class_id &&
                iou(candidates[i], candidates[j]) > iou_threshold)
                suppressed[j] = true;
        }
    }
    return result;
}

std::vector<Detection> process_with_timing(
    const std::vector<rknn_output>& outputs,
    const std::vector<rknn_tensor_attr>& attrs,
    int model_width,
    int model_height,
    int orig_width,
    int orig_height,
    float conf_threshold,
    float iou_threshold,
    const std::unordered_set<std::string>& target_set,
    double& scan_ms,
    double& dfl_ms,
    double& box_ms,
    double& nms_ms) {

    float scale = std::min(static_cast<float>(model_width) / orig_width,
                           static_cast<float>(model_height) / orig_height);
    int resized_width = static_cast<int>(std::round(orig_width * scale));
    int resized_height = static_cast<int>(std::round(orig_height * scale));
    float pad_x = static_cast<float>((model_width - resized_width) / 2);
    float pad_y = static_cast<float>((model_height - resized_height) / 2);
    std::vector<Detection> candidates;
    candidates.reserve(64);

    double total_scan = 0.0;
    double total_dfl = 0.0;
    double total_box = 0.0;

    for (int branch = 0; branch < kBranches; ++branch) {
        int box_index = branch * kOutputsPerBranch;
        int score_index = box_index + 1;
        int sum_index = box_index + 2;
        const auto& box_attr = attrs[box_index];
        const auto& score_attr = attrs[score_index];
        const auto& sum_attr = attrs[sum_index];
        int num_classes = static_cast<int>(score_attr.dims[1]);

        int grid_h = static_cast<int>(box_attr.dims[2]);
        int grid_w = static_cast<int>(box_attr.dims[3]);
        int grid_len = grid_h * grid_w;
        int dfl_len = static_cast<int>(box_attr.dims[1]) / 4;
        int stride = model_height / grid_h;
        const int8_t* box_tensor =
            static_cast<const int8_t*>(outputs[box_index].buf);
        const int8_t* score_tensor =
            static_cast<const int8_t*>(outputs[score_index].buf);
        const int8_t* sum_tensor =
            static_cast<const int8_t*>(outputs[sum_index].buf);
        int8_t score_threshold = quantize_i8(conf_threshold, score_attr);
        int8_t sum_threshold = quantize_i8(conf_threshold, sum_attr);

        int8_t local_scores[6400];
        int16_t local_classes[6400];
        std::vector<int8_t> best_scores_buf;
        std::vector<int16_t> best_classes_buf;
        int8_t* best_scores;
        int16_t* best_classes;
        if (grid_len <= 6400) {
            best_scores = local_scores;
            best_classes = local_classes;
        } else {
            best_scores_buf.assign(grid_len, -128);
            best_classes_buf.assign(grid_len, -1);
            best_scores = best_scores_buf.data();
            best_classes = best_classes_buf.data();
        }
        std::fill_n(best_scores, grid_len, -128);
        std::fill_n(best_classes, grid_len, -1);

        // Stage 1: Candidate scanning / Class filtering
        auto t_scan_0 = std::chrono::steady_clock::now();
        for (int class_id = 0; class_id < num_classes; ++class_id) {
            const int8_t* class_scores = score_tensor + class_id * grid_len;
            for (int cell = 0; cell < grid_len; ++cell) {
                if (sum_tensor[cell] < sum_threshold) continue;
                int8_t score = class_scores[cell];
                if (score > score_threshold && score > best_scores[cell]) {
                    best_scores[cell] = score;
                    best_classes[cell] = class_id;
                }
            }
        }
        auto t_scan_1 = std::chrono::steady_clock::now();
        total_scan += std::chrono::duration<double, std::milli>(t_scan_1 - t_scan_0).count();

        // Stage 2 & 3: DFL decoding & Bounding box coordinate mapping
        for (int row = 0; row < grid_h; ++row) {
            for (int col = 0; col < grid_w; ++col) {
                int cell = row * grid_w + col;
                int best_class = best_classes[cell];
                if (best_class < 0) continue;

                const char* label = num_classes == kDesktopClasses
                    ? kDesktopLabels[best_class] : COCO_LABELS[best_class];
                if (!target_set.empty() &&
                    target_set.find(label) == target_set.end())
                    continue;

                // Stage 2: DFL decoding
                auto t_dfl_0 = std::chrono::steady_clock::now();
                std::array<float, kMaxDflValues> dfl_logits{};
                for (int i = 0; i < 4 * dfl_len; ++i)
                    dfl_logits[i] =
                        dequantize(box_tensor[i * grid_len + cell], box_attr);
                float distances[4];
                compute_dfl(dfl_logits.data(), dfl_len, distances);
                auto t_dfl_1 = std::chrono::steady_clock::now();
                total_dfl += std::chrono::duration<double, std::milli>(t_dfl_1 - t_dfl_0).count();

                // Stage 3: Coordinate mapping & detection box construction
                auto t_box_0 = std::chrono::steady_clock::now();
                Detection det;
                det.x1 = ((col + 0.5f - distances[0]) * stride - pad_x) / scale;
                det.y1 = ((row + 0.5f - distances[1]) * stride - pad_y) / scale;
                det.x2 = ((col + 0.5f + distances[2]) * stride - pad_x) / scale;
                det.y2 = ((row + 0.5f + distances[3]) * stride - pad_y) / scale;
                det.x1 = std::clamp(det.x1, 0.0f, static_cast<float>(orig_width));
                det.y1 = std::clamp(det.y1, 0.0f, static_cast<float>(orig_height));
                det.x2 = std::clamp(det.x2, 0.0f, static_cast<float>(orig_width));
                det.y2 = std::clamp(det.y2, 0.0f, static_cast<float>(orig_height));
                det.class_id = best_class;
                det.label = label;
                det.score = dequantize(best_scores[cell], score_attr);
                candidates.push_back(std::move(det));
                auto t_box_1 = std::chrono::steady_clock::now();
                total_box += std::chrono::duration<double, std::milli>(t_box_1 - t_box_0).count();
            }
        }
    }

    // Stage 4: NMS
    auto t_nms_0 = std::chrono::steady_clock::now();
    auto result = run_nms(std::move(candidates), iou_threshold);
    auto t_nms_1 = std::chrono::steady_clock::now();

    scan_ms = total_scan;
    dfl_ms = total_dfl;
    box_ms = total_box;
    nms_ms = std::chrono::duration<double, std::milli>(t_nms_1 - t_nms_0).count();

    return result;
}

}  // namespace

int main(int argc, char** argv) {
    rknn_context ctx = 0;
    rknn_tensor_mem* input_mem = nullptr;
    try {
        Options opts = parse_args(argc, argv);
        std::vector<uint8_t> model = read_file(opts.model_path);

        uint32_t flags = opts.enable_sram ? RKNN_FLAG_ENABLE_SRAM : 0;
        if (opts.perf_detail) flags |= RKNN_FLAG_COLLECT_PERF_MASK;
        check_rknn(rknn_init(&ctx, model.data(), static_cast<uint32_t>(model.size()),
                             flags, nullptr),
                   "rknn_init");
        check_rknn(rknn_set_core_mask(ctx, opts.core_mask), "rknn_set_core_mask");

        rknn_sdk_version version{};
        check_rknn(rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &version, sizeof(version)),
                   "RKNN_QUERY_SDK_VERSION");

        rknn_input_output_num io_num{};
        check_rknn(rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)),
                   "RKNN_QUERY_IN_OUT_NUM");
        if (io_num.n_input != 1)
            throw std::runtime_error("benchmark currently requires exactly one input");

        rknn_tensor_attr input_attr{};
        input_attr.index = 0;
        check_rknn(rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attr, sizeof(input_attr)),
                   "RKNN_QUERY_INPUT_ATTR");

        std::vector<rknn_tensor_attr> output_attrs(io_num.n_output);
        for (uint32_t i = 0; i < io_num.n_output; ++i) {
            output_attrs[i].index = i;
            check_rknn(rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i],
                                  sizeof(output_attrs[i])),
                       "RKNN_QUERY_OUTPUT_ATTR");
        }

        if (opts.postprocess == "current") {
            if (io_num.n_output != 2 || opts.output_mode != "float")
                throw std::runtime_error(
                    "current postprocess requires 2 outputs and --output-mode float");
        } else if (opts.postprocess == "rockchip") {
            if (io_num.n_output != 9 || opts.output_mode != "raw")
                throw std::runtime_error(
                    "rockchip postprocess requires 9 outputs and --output-mode raw");
        }

        std::cout << "model=" << opts.model_path
                  << " bytes=" << model.size()
                  << " warmup=" << opts.warmup
                  << " runs=" << opts.runs
                  << " period_ms=" << opts.period_ms
                  << " core=" << opts.core_name
                  << " sram=" << (opts.enable_sram ? "on" : "off")
                  << " input_mode=" << opts.input_mode
                  << " output_mode=" << opts.output_mode
                  << " postprocess=" << opts.postprocess
                  << " perf_query=" << (opts.query_perf ? "on" : "off")
                  << " perf_detail=" << (opts.perf_detail ? "on" : "off") << '\n';
        std::cout << "api=" << version.api_version << " driver=" << version.drv_version << '\n';
        std::cout << "inputs=" << io_num.n_input << " outputs=" << io_num.n_output << '\n';
        print_tensor("input", 0, input_attr);

        for (uint32_t i = 0; i < io_num.n_output; ++i)
            print_tensor("output", i, output_attrs[i]);

        std::vector<uint8_t> input;
        if (opts.input_path.empty()) {
            input.resize(input_attr.size);
            for (size_t i = 0; i < input.size(); ++i)
                input[i] = static_cast<uint8_t>((i * 131U + 17U) & 0xffU);
            std::cout << "input_source=deterministic_pattern\n";
        } else {
            input = read_file(opts.input_path);
            if (input.size() != input_attr.size) {
                throw std::runtime_error("input file size " + std::to_string(input.size()) +
                                         " does not match tensor size " +
                                         std::to_string(input_attr.size));
            }
            std::cout << "input_source=" << opts.input_path << '\n';
        }

        rknn_input rknn_in{};
        rknn_in.index = 0;
        rknn_in.buf = input.data();
        rknn_in.size = static_cast<uint32_t>(input.size());
        rknn_in.pass_through = 0;
        rknn_in.type = RKNN_TENSOR_UINT8;
        rknn_in.fmt = RKNN_TENSOR_NHWC;

        if (opts.input_mode == "zero-copy") {
            uint32_t mem_size = input_attr.size_with_stride > 0
                ? input_attr.size_with_stride
                : input_attr.size;
            input_mem = rknn_create_mem(ctx, mem_size);
            if (!input_mem) throw std::runtime_error("rknn_create_mem failed");
            std::memset(input_mem->virt_addr, 0, input_mem->size);

            rknn_tensor_attr bound_attr = input_attr;
            bound_attr.pass_through = 0;
            bound_attr.type = RKNN_TENSOR_UINT8;
            bound_attr.fmt = RKNN_TENSOR_NHWC;
            check_rknn(rknn_set_io_mem(ctx, input_mem, &bound_attr), "rknn_set_io_mem");
            std::cout << "input_dma_bytes=" << input_mem->size << '\n';
        }

        if (opts.prealloc_outputs && opts.output_mode != "raw")
            throw std::runtime_error("--prealloc-outputs requires --output-mode raw");
        std::vector<std::vector<uint8_t>> output_buffers;
        if (opts.prealloc_outputs) {
            output_buffers.resize(io_num.n_output);
            for (uint32_t i = 0; i < io_num.n_output; ++i)
                output_buffers[i].resize(output_attrs[i].size);
        }

        std::vector<rknn_output> outputs(io_num.n_output);
        for (uint32_t i = 0; i < io_num.n_output; ++i) {
            outputs[i].index = i;
            outputs[i].want_float = opts.output_mode == "float";
            outputs[i].is_prealloc = opts.prealloc_outputs ? 1 : 0;
            if (opts.prealloc_outputs) {
                outputs[i].buf = output_buffers[i].data();
                outputs[i].size = static_cast<uint32_t>(output_buffers[i].size());
            }
        }

        YoloConfig yolo_config;
        yolo_config.conf_threshold = opts.conf_threshold;
        yolo_config.iou_threshold = opts.iou_threshold;
        yolo_config.model_input_size = static_cast<int>(input_attr.dims[1]);
        YoloPostprocess current_postprocess(yolo_config);
        RockchipYoloPostprocess rockchip_postprocess(yolo_config);
        if (opts.postprocess == "rockchip") {
            std::string validation_error;
            if (!rockchip_postprocess.validate(
                    output_attrs,
                    static_cast<int>(input_attr.dims[2]),
                    static_cast<int>(input_attr.dims[1]),
                    &validation_error))
                throw std::runtime_error(
                    "invalid Rockchip outputs: " + validation_error);
        }
        std::vector<Detection> last_detections;
        std::string perf_detail_report;
        std::vector<double> scan_times;
        std::vector<double> dfl_times;
        std::vector<double> box_times;
        std::vector<double> nms_times;

        auto run_once = [&](bool measured,
                            std::vector<double>& set_times,
                            std::vector<double>& run_times,
                            std::vector<double>& get_times,
                            std::vector<double>& postprocess_times,
                            std::vector<double>& total_times,
                            std::vector<double>& perf_times) {
            auto t0 = std::chrono::steady_clock::now();
            if (opts.input_mode == "zero-copy") {
                std::memcpy(input_mem->virt_addr, input.data(), input.size());
            } else {
                check_rknn(rknn_inputs_set(ctx, 1, &rknn_in), "rknn_inputs_set");
            }
            auto t1 = std::chrono::steady_clock::now();
            check_rknn(rknn_run(ctx, nullptr), "rknn_run");
            auto t2 = std::chrono::steady_clock::now();

            rknn_perf_run perf{};
            int perf_ret = opts.query_perf
                ? rknn_query(ctx, RKNN_QUERY_PERF_RUN, &perf, sizeof(perf))
                : -1;

            check_rknn(rknn_outputs_get(ctx, io_num.n_output, outputs.data(), nullptr),
                       "rknn_outputs_get");
            auto t3 = std::chrono::steady_clock::now();

            if (measured && opts.perf_detail && perf_detail_report.empty()) {
                rknn_perf_detail detail{};
                check_rknn(rknn_query(ctx, RKNN_QUERY_PERF_DETAIL, &detail, sizeof(detail)),
                           "RKNN_QUERY_PERF_DETAIL");
                if (detail.perf_data && detail.data_len > 0)
                    perf_detail_report.assign(detail.perf_data, detail.data_len);
            }

            if (opts.postprocess == "current") {
                last_detections = current_postprocess.process(
                    static_cast<const float*>(outputs[0].buf),
                    static_cast<const float*>(outputs[1].buf),
                    static_cast<int>(output_attrs[0].dims[2]),
                    static_cast<int>(input_attr.dims[1]),
                    static_cast<int>(input_attr.dims[2]));
            } else if (opts.postprocess == "rockchip") {
                if (opts.postprocess_stages) {
                    double scan_ms = 0.0, dfl_ms = 0.0, box_ms = 0.0, nms_ms = 0.0;
                    std::unordered_set<std::string> target_set;
                    for (const auto& target : yolo_config.target_classes)
                        target_set.insert(target);
                    last_detections = process_with_timing(
                        outputs, output_attrs,
                        static_cast<int>(input_attr.dims[2]),
                        static_cast<int>(input_attr.dims[1]),
                        static_cast<int>(input_attr.dims[2]),
                        static_cast<int>(input_attr.dims[1]),
                        opts.conf_threshold, opts.iou_threshold,
                        target_set,
                        scan_ms, dfl_ms, box_ms, nms_ms);
                    if (measured) {
                        scan_times.push_back(scan_ms);
                        dfl_times.push_back(dfl_ms);
                        box_times.push_back(box_ms);
                        nms_times.push_back(nms_ms);
                    }
                } else {
                    last_detections = rockchip_postprocess.process(
                        outputs, output_attrs,
                        static_cast<int>(input_attr.dims[2]),
                        static_cast<int>(input_attr.dims[1]),
                        static_cast<int>(input_attr.dims[2]),
                        static_cast<int>(input_attr.dims[1]));
                }
            }
            auto t4 = std::chrono::steady_clock::now();
            check_rknn(rknn_outputs_release(ctx, io_num.n_output, outputs.data()),
                       "rknn_outputs_release");

            if (measured) {
                set_times.push_back(elapsed_ms(t0, t1));
                run_times.push_back(elapsed_ms(t1, t2));
                get_times.push_back(elapsed_ms(t2, t3));
                if (opts.postprocess != "none") {
                    postprocess_times.push_back(elapsed_ms(t3, t4));
                    total_times.push_back(elapsed_ms(t1, t4));
                }
                if (perf_ret == RKNN_SUCC)
                    perf_times.push_back(static_cast<double>(perf.run_duration) / 1000.0);
            }

            if (opts.period_ms > 0)
                std::this_thread::sleep_until(t0 + std::chrono::milliseconds(opts.period_ms));
        };

        std::vector<double> unused;
        for (int i = 0; i < opts.warmup; ++i)
            run_once(false, unused, unused, unused, unused, unused, unused);

        std::vector<double> set_times;
        std::vector<double> run_times;
        std::vector<double> get_times;
        std::vector<double> postprocess_times;
        std::vector<double> total_times;
        std::vector<double> perf_times;
        set_times.reserve(opts.runs);
        run_times.reserve(opts.runs);
        get_times.reserve(opts.runs);
        postprocess_times.reserve(opts.runs);
        total_times.reserve(opts.runs);
        perf_times.reserve(opts.runs);
        if (opts.postprocess_stages) {
            scan_times.reserve(opts.runs);
            dfl_times.reserve(opts.runs);
            box_times.reserve(opts.runs);
            nms_times.reserve(opts.runs);
        }

        for (int i = 0; i < opts.runs; ++i)
            run_once(true, set_times, run_times, get_times, postprocess_times,
                     total_times, perf_times);

        std::cout << "--- results ---\n";
        print_summary(opts.input_mode == "zero-copy" ? "input_dma_copy" : "inputs_set",
                      summarize(set_times));
        print_summary("rknn_run_wall", summarize(run_times));
        print_summary("outputs_get", summarize(get_times));
        if (!postprocess_times.empty()) {
            print_summary("postprocess", summarize(postprocess_times));
            print_summary("run_get_post", summarize(total_times));
            if (opts.postprocess_stages && !scan_times.empty()) {
                print_summary("  stage_scan", summarize(scan_times));
                print_summary("  stage_dfl", summarize(dfl_times));
                print_summary("  stage_box_map", summarize(box_times));
                print_summary("  stage_nms", summarize(nms_times));
            }
        }
        if (!perf_times.empty())
            print_summary("rknn_perf_run", summarize(perf_times));
        else
            std::cout << "rknn_perf_run unavailable\n";
        if (!perf_detail_report.empty())
            std::cout << "--- rknn_perf_detail ---\n" << perf_detail_report;
        if (opts.dump_detections) print_detections(last_detections);

        if (input_mem) {
            rknn_destroy_mem(ctx, input_mem);
            input_mem = nullptr;
        }
        rknn_destroy(ctx);
        return 0;
    } catch (const std::exception& e) {
        if (input_mem && ctx) rknn_destroy_mem(ctx, input_mem);
        if (ctx) rknn_destroy(ctx);
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
