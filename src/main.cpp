#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

#include <nlohmann/json.hpp>

#include "app/ChannelPipeline.hpp"
#include "infer/NpuInferenceScheduler.hpp"
#include "infer/RknnNpuExecutor.hpp"

using json = nlohmann::json;

namespace {

constexpr const char* kResultRtmpPrefix = "rtmp://192.168.0.168/live/alertgateway_channel_";
std::atomic<bool> g_running{true};

void on_signal(int) {
    g_running = false;
}

void merge_json(json& base, const json& override_cfg) {
    if (!base.is_object() || !override_cfg.is_object()) {
        base = override_cfg;
        return;
    }
    for (const auto& [key, value] : override_cfg.items()) {
        if (base.contains(key) && base[key].is_object() && value.is_object()) {
            merge_json(base[key], value);
        } else {
            base[key] = value;
        }
    }
}

json load_entry_config(const std::filesystem::path& entry_config_path) {
    std::ifstream input(entry_config_path);
    if (!input) {
        throw std::runtime_error("Cannot open config: " + entry_config_path.string());
    }
    json cfg;
    input >> cfg;

    // 旧入口继续支持 active_config；选择的配置覆盖入口中的同名字段。
    if (cfg.contains("active_config")) {
        const std::string selected_name = cfg.at("active_config").get<std::string>();
        std::filesystem::path selected_path(selected_name);
        if (selected_path.is_relative()) {
            selected_path = entry_config_path.parent_path() / selected_path;
        }
        std::ifstream selected_file(selected_path);
        if (!selected_file) {
            throw std::runtime_error("Cannot open selected config: " + selected_path.string());
        }
        json selected_cfg;
        selected_file >> selected_cfg;
        merge_json(cfg, selected_cfg);
        std::cout << "[Config] active_config=" << selected_path.string() << "\n";
    }
    return cfg;
}

struct NormalizedChannel {
    std::string id;
    json config;
};

std::vector<NormalizedChannel> normalize_channels(const json& root_config) {
    if (!root_config.contains("channels")) {
        // 保持旧平面配置的启动方式与字段完全兼容。
        return {{"single", root_config}};
    }

    const auto& channels = root_config.at("channels");
    if (!channels.is_array() || channels.empty()) {
        throw std::runtime_error("Config error: channels must be a non-empty array");
    }

    json common = root_config;
    common.erase("channels");
    common.erase("active_config");
    std::unordered_set<std::string> ids;
    std::vector<NormalizedChannel> normalized;
    normalized.reserve(channels.size());

    for (const auto& channel : channels) {
        if (!channel.is_object()) {
            throw std::runtime_error("Config error: every channels item must be an object");
        }
        const std::string id = channel.value("id", std::string{});
        if (id.empty()) {
            throw std::runtime_error("Config error: every channels item requires a non-empty id");
        }
        if (!ids.insert(id).second) {
            throw std::runtime_error("Config error: duplicate channel id: " + id);
        }
        json effective = common;
        merge_json(effective, channel);
        effective.erase("id");
        normalized.push_back({id, std::move(effective)});
    }
    return normalized;
}

void validate_output_topology(const std::vector<NormalizedChannel>& channels, bool multi_channel_mode) {
    std::unordered_set<std::string> result_urls;
    for (const auto& channel : channels) {
        const json stream = channel.config.value("stream", json::object());
        const std::string sink = stream.value("sink", std::string("fixed_rtmp"));
        if (sink == "fixed_rtmp") {
            const std::string target = stream.value("rtmp_url", std::string{});
            const std::string expected = multi_channel_mode
                ? std::string(kResultRtmpPrefix) + channel.id
                : "rtmp://192.168.0.168/live/alertgateway";
            if (target != expected) {
                throw std::runtime_error(
                    "Config error: channel " + channel.id +
                    " fixed_rtmp must use " + expected);
            }
            if (!result_urls.insert(target).second) {
                throw std::runtime_error("Config error: duplicate RTMP result output: " + target);
            }
        } else if (sink == "local_flv") {
            const std::string path = stream.value("path", stream.value("rtmp_url", std::string{}));
            if (path.empty()) {
                throw std::runtime_error("Config error: channel " + channel.id + " local_flv requires stream.path");
            }
        } else if (sink != "metrics_only") {
            throw std::runtime_error("Config error: channel " + channel.id + " has unsupported stream.sink");
        }
    }
}

NpuSchedulerConfig parse_npu_scheduler_config(const json& root_config) {
    const json scheduler = root_config.value("npu_scheduler", json::object());
    if (!scheduler.is_object()) {
        throw std::runtime_error("Config error: npu_scheduler must be an object");
    }

    NpuSchedulerConfig config;
    config.enabled = scheduler.value("enabled", false);
    config.mode = scheduler.value("mode", std::string("global_serial"));
    config.core_mask = scheduler.value("core_mask", std::string("0_1_2"));
    config.global_target_fps = scheduler.value("global_target_fps", 0);
    config.max_frame_age_ms = scheduler.value("max_frame_age_ms", 250);
    config.stats_interval_sec = scheduler.value("stats_interval_sec", 10);
    return config;
}

bool image_processing_enabled(const json& channel_config) {
    const json image = channel_config.value("image_processing", json::object());
    const auto enabled = [&image](const char* key) {
        return image.value(key, json::object()).value("enabled", false);
    };
    return enabled("thumbnail") || enabled("roi") || enabled("tiling");
}

void validate_npu_scheduler_config(const NpuSchedulerConfig& scheduler,
                                   const std::vector<NormalizedChannel>& channels) {
    if (!scheduler.enabled) return;

    if (scheduler.mode != "global_serial") {
        throw std::runtime_error("Config error: npu_scheduler.mode must be global_serial");
    }
    if (scheduler.core_mask != "0_1_2") {
        throw std::runtime_error("Config error: npu_scheduler.core_mask must be 0_1_2 in P0");
    }
    if (scheduler.global_target_fps <= 0 || scheduler.max_frame_age_ms < 0 ||
        scheduler.stats_interval_sec <= 0) {
        throw std::runtime_error("Config error: invalid npu_scheduler budget, frame age or stats interval");
    }

    std::string model_path;
    std::string output_layout;
    int model_input_width = 640;
    int model_input_height = 640;
    int requested_fps = 0;
    for (const auto& channel : channels) {
        const json model = channel.config.value("model", json::object());
        const std::string path = model.value("path", std::string{});
        const std::string layout = model.value("output_layout", std::string("decoded"));
        const int input_width = model.value("input_width", 640);
        const int input_height = model.value("input_height", 640);
        const int target_fps = model.value("target_infer_fps", 0);
        const int weight = model.value("npu_weight", 1);

        if (path.empty() || target_fps <= 0 || weight < 1) {
            throw std::runtime_error(
                "Config error: channel " + channel.id +
                " requires model.path, target_infer_fps > 0 and npu_weight >= 1 when scheduler is enabled");
        }
        if (image_processing_enabled(channel.config)) {
            throw std::runtime_error(
                "Config error: channel " + channel.id +
                " must disable thumbnail, ROI and tiling for the first scheduler implementation");
        }
        if (model_path.empty()) {
            model_path = path;
            output_layout = layout;
            model_input_width = input_width;
            model_input_height = input_height;
        } else if (path != model_path || layout != output_layout ||
                   input_width != model_input_width || input_height != model_input_height) {
            throw std::runtime_error(
                "Config error: all scheduler channels must use the same model path, output layout and input contract");
        }
        requested_fps += target_fps;
    }
    if (requested_fps > scheduler.global_target_fps) {
        throw std::runtime_error(
            "Config error: sum of model.target_infer_fps exceeds npu_scheduler.global_target_fps");
    }
}

ModelConfig executor_model_config(const NormalizedChannel& channel) {
    const json model = channel.config.at("model");
    const json detection = channel.config.at("detection");
    ModelConfig config;
    config.channel_id = "npu-executor";
    config.path = model.at("path").get<std::string>();
    config.conf_threshold = model.at("conf_threshold").get<float>();
    config.iou_threshold = model.at("iou_threshold").get<float>();
    config.target_classes = detection.at("target_classes").get<std::vector<std::string>>();
    config.output_layout = model.value("output_layout", std::string("decoded"));
    return config;
}

class FFmpegNetworkGuard {
public:
    FFmpegNetworkGuard() {
        const int ret = avformat_network_init();
        if (ret < 0) {
            throw std::runtime_error("avformat_network_init failed");
        }
    }
    ~FFmpegNetworkGuard() {
        avformat_network_deinit();
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config.json>\n";
        return 1;
    }

    try {
        const json root_config = load_entry_config(std::filesystem::path(argv[1]));
        const auto channels = normalize_channels(root_config);
        validate_output_topology(channels, root_config.contains("channels"));
        const NpuSchedulerConfig scheduler_config = parse_npu_scheduler_config(root_config);
        validate_npu_scheduler_config(scheduler_config, channels);

        std::cout << "[Config] channels=" << channels.size() << "\n";
        FFmpegNetworkGuard ffmpeg_network;
        std::shared_ptr<RknnNpuExecutor> npu_executor;
        NpuInferenceSchedulerPtr npu_scheduler;
        if (scheduler_config.enabled) {
            npu_executor = std::make_shared<RknnNpuExecutor>(executor_model_config(channels.front()));
            std::string executor_error;
            if (!npu_executor->start(&executor_error)) {
                throw std::runtime_error("NPU executor startup failed: " + executor_error);
            }
            npu_scheduler = std::make_shared<NpuInferenceScheduler>(scheduler_config);
            npu_scheduler->set_executor([npu_executor](const NpuChannelConfig& channel, const Frame& frame) {
                return npu_executor->execute(channel, frame);
            });
            npu_scheduler->start();
            std::cout << "[NpuScheduler] mode=global_serial core_mask=0_1_2 global_target_fps="
                      << scheduler_config.global_target_fps << "\n";
        }

        std::vector<std::unique_ptr<ChannelPipeline>> pipelines;
        pipelines.reserve(channels.size());
        for (const auto& channel : channels) {
            std::cout << "[Config] channel=" << channel.id << "\n";
            pipelines.push_back(std::make_unique<ChannelPipeline>(channel.id, channel.config, npu_scheduler));
        }

        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);

        try {
            for (auto& pipeline : pipelines) {
                pipeline->start();
            }
        } catch (...) {
            for (auto it = pipelines.rbegin(); it != pipelines.rend(); ++it) {
                (*it)->stop();
            }
            if (npu_scheduler) npu_scheduler->stop();
            if (npu_executor) npu_executor->stop();
            throw;
        }

        std::cout << "AlertGateway running with " << pipelines.size()
                  << " channel(s) — press Ctrl+C to stop\n" << std::flush;
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nShutting down...\n";
        for (auto& pipeline : pipelines) {
            pipeline->stop_source();
        }
        for (auto it = pipelines.rbegin(); it != pipelines.rend(); ++it) {
            (*it)->stop();
        }
        if (npu_scheduler) npu_scheduler->stop();
        if (npu_executor) npu_executor->stop();
        std::cout << "Done.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "AlertGateway startup failed: " << e.what() << "\n";
        return 1;
    }
}
