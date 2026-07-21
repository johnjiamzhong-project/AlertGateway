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

        std::cout << "[Config] channels=" << channels.size() << "\n";
        std::vector<std::unique_ptr<ChannelPipeline>> pipelines;
        pipelines.reserve(channels.size());
        for (const auto& channel : channels) {
            std::cout << "[Config] channel=" << channel.id << "\n";
            pipelines.push_back(std::make_unique<ChannelPipeline>(channel.id, channel.config));
        }

        FFmpegNetworkGuard ffmpeg_network;
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
        std::cout << "Done.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "AlertGateway startup failed: " << e.what() << "\n";
        return 1;
    }
}
