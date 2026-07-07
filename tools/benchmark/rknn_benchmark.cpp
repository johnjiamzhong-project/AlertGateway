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
    rknn_core_mask core_mask = RKNN_NPU_CORE_AUTO;
    std::string core_name = "auto";
    bool enable_sram = false;
    bool dump_detections = false;
    float conf_threshold = 0.25f;
    float iou_threshold = 0.45f;
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
        << "  --core auto|0|1|2|01|012|all\n"
        << "  --sram                   Enable RKNN_FLAG_ENABLE_SRAM\n"
        << "  --input-mode MODE        standard or zero-copy (default: standard)\n"
        << "  --output-mode MODE       raw or float (default: raw)\n"
        << "  --postprocess MODE       none, current, or rockchip (default: none)\n"
        << "  --conf-threshold FLOAT   Detection threshold (default: 0.25)\n"
        << "  --iou-threshold FLOAT    NMS IoU threshold (default: 0.45)\n"
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
        else if (arg == "--no-perf-query") opts.query_perf = false;
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

}  // namespace

int main(int argc, char** argv) {
    rknn_context ctx = 0;
    rknn_tensor_mem* input_mem = nullptr;
    try {
        Options opts = parse_args(argc, argv);
        std::vector<uint8_t> model = read_file(opts.model_path);

        uint32_t flags = opts.enable_sram ? RKNN_FLAG_ENABLE_SRAM : 0;
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
                  << " perf_query=" << (opts.query_perf ? "on" : "off") << '\n';
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

        std::vector<rknn_output> outputs(io_num.n_output);
        for (uint32_t i = 0; i < io_num.n_output; ++i) {
            outputs[i].index = i;
            outputs[i].want_float = opts.output_mode == "float";
            outputs[i].is_prealloc = 0;
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

            if (opts.postprocess == "current") {
                last_detections = current_postprocess.process(
                    static_cast<const float*>(outputs[0].buf),
                    static_cast<const float*>(outputs[1].buf),
                    static_cast<int>(output_attrs[0].dims[2]),
                    static_cast<int>(input_attr.dims[1]),
                    static_cast<int>(input_attr.dims[2]));
            } else if (opts.postprocess == "rockchip") {
                last_detections = rockchip_postprocess.process(
                    outputs, output_attrs,
                    static_cast<int>(input_attr.dims[2]),
                    static_cast<int>(input_attr.dims[1]),
                    static_cast<int>(input_attr.dims[2]),
                    static_cast<int>(input_attr.dims[1]));
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
        }
        if (!perf_times.empty())
            print_summary("rknn_perf_run", summarize(perf_times));
        else
            std::cout << "rknn_perf_run unavailable\n";
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
