#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>

#include "common/BlockingQueue.hpp"
#include "common/Frame.hpp"
#include "common/SharedDetections.hpp"
#include "capture/IVideoSource.hpp"
#include "capture/CaptureThread.hpp"
#include "capture/PullStreamThread.hpp"
#include "infer/InferThread.hpp"
#include <memory>
#include "encode/EncodeThread.hpp"
#include "stream/StreamThread.hpp"
#include "mqtt/MqttThread.hpp"

using json = nlohmann::json;

static std::atomic<bool> g_running{true};

static void on_signal(int) { g_running = false; }

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <config.json>\n";
        return 1;
    }

    std::ifstream f(argv[1]);
    if (!f) { std::cerr << "Cannot open config: " << argv[1] << "\n"; return 1; }
    json cfg;
    f >> cfg;

    // 视频源配置：V4L2 本地摄像头或 FFmpeg 拉流
    std::string source_type = "v4l2";
    int src_w = 640;
    int src_h = 480;
    int src_fps = 15;

    CameraConfig cam_cfg{};
    PullStreamConfig pull_cfg{};

    if (cfg.contains("source")) {
        source_type = cfg["source"].value("type", "v4l2");
        src_w = cfg["source"]["width"].get<int>();
        src_h = cfg["source"]["height"].get<int>();
        src_fps = cfg["source"]["fps"].get<int>();
        if (source_type != "v4l2" && source_type != "pull_stream") {
            std::cerr << "Config error: source.type must be 'v4l2' or 'pull_stream'\n";
            return 1;
        }
        if (src_w <= 0 || src_h <= 0 || src_fps <= 0 || (src_w & 1) || (src_h & 1)) {
            std::cerr << "Config error: source width/height must be positive even values and fps must be positive\n";
            return 1;
        }

        if (source_type == "pull_stream") {
            pull_cfg.url = cfg["source"].value("url", std::string{});
            if (pull_cfg.url.empty()) {
                std::cerr << "Config error: source.url is required for pull_stream\n";
                return 1;
            }
            pull_cfg.width = src_w;
            pull_cfg.height = src_h;
            pull_cfg.fps = src_fps;
            pull_cfg.reconnect_sec = cfg["source"].value("reconnect_sec", 5);
        } else {
            cam_cfg.device = cfg["source"]["device"].get<std::string>();
            cam_cfg.width = src_w;
            cam_cfg.height = src_h;
            cam_cfg.fps = src_fps;
        }
    } else if (cfg.contains("camera")) {
        // 向下兼容旧 camera 字段
        source_type = "v4l2";
        cam_cfg.device = cfg["camera"]["device"].get<std::string>();
        cam_cfg.width = cfg["camera"]["width"].get<int>();
        cam_cfg.height = cfg["camera"]["height"].get<int>();
        cam_cfg.fps = cfg["camera"]["fps"].get<int>();
        src_w = cam_cfg.width;
        src_h = cam_cfg.height;
        src_fps = cam_cfg.fps;
    } else {
        std::cerr << "Config error: no 'source' or 'camera' node found in configuration\n";
        return 1;
    }

    // 推理配置：RKNN 模型路径、置信度阈值、NMS IoU 阈值、目标类别过滤、推理抽帧间隔
    ModelConfig model_cfg{
        cfg["model"]["path"].get<std::string>(),
        cfg["model"]["conf_threshold"].get<float>(),
        cfg["model"]["iou_threshold"].get<float>(),
        cfg["detection"]["target_classes"].get<std::vector<std::string>>(),
        cfg["model"].value("infer_every_n_frames", 1),
        cfg["model"].value("output_layout", std::string("decoded"))
    };

    // 检测上报配置：MQTT 上报周期（秒），结果有变化时才上报
    DetectionConfig det_cfg{
        cfg["detection"]["report_interval_sec"].get<int>()
    };
    ImageProcessingConfig image_cfg;
    const json ip = cfg.value("image_processing", json::object());
    if (ip.contains("thumbnail")) {
        const auto& t = ip["thumbnail"];
        image_cfg.thumbnail.enabled = t.value("enabled", false);
        image_cfg.thumbnail.width = t.value("width", 320);
        image_cfg.thumbnail.height = t.value("height", 180);
        image_cfg.thumbnail.on_detection_only = t.value("on_detection_only", true);
    }
    if (ip.contains("roi")) {
        const auto& r = ip["roi"];
        image_cfg.roi.enabled = r.value("enabled", false);
        image_cfg.roi.filter_outside = r.value("filter_outside", true);
        image_cfg.roi.track_dwell_sec = r.value("track_dwell_sec", 0.0f);
        for (const auto& item : r.value("regions", json::array())) {
            image_cfg.roi.regions.push_back({
                item.value("id", std::string("roi-") + std::to_string(image_cfg.roi.regions.size())),
                item.value("x", 0.0f), item.value("y", 0.0f),
                item.value("w", 1.0f), item.value("h", 1.0f)
            });
        }
    }
    if (ip.contains("tiling")) {
        const auto& t = ip["tiling"];
        image_cfg.tiling.enabled = t.value("enabled", false);
        image_cfg.tiling.grid_cols = t.value("grid_cols", 2);
        image_cfg.tiling.grid_rows = t.value("grid_rows", 1);
        image_cfg.tiling.overlap_ratio = t.value("overlap_ratio", 0.10f);
        image_cfg.tiling.merge_iou_threshold = t.value("merge_iou_threshold", 0.45f);
    }

    // 编码配置：分辨率和帧率复用视频源配置；码率和文字标签可独立调节
    EncodeConfig enc_cfg{
        src_w,
        src_h,
        src_fps,
        cfg["stream"].value("bitrate_kbps", 2000),
        cfg["stream"].value("draw_detection_labels", true)
    };

    // 推流配置：RTMP 地址 + 分辨率/帧率（用于 FFmpeg AVStream 参数）
    StreamConfig stream_cfg{
        cfg["stream"]["rtmp_url"].get<std::string>(),
        src_w,
        src_h,
        src_fps
    };

    // MQTT 配置：Broker 地址、端口、发布 Topic、客户端 ID
    MqttConfig mqtt_cfg{
        cfg["mqtt"]["broker"].get<std::string>(),
        cfg["mqtt"]["port"].get<int>(),
        cfg["mqtt"]["topic"].get<std::string>(),
        cfg["mqtt"]["client_id"].get<std::string>()
    };

    // ── Pipeline 拓扑 ────────────────────────────────────────────────────────
    //
    //  CaptureThread ──→ [enc_queue]   → EncodeThread（YUYV→NV12→MPP 硬编→叠检测框）
    //                └──→ [infer_queue] → InferThread（RKNN 推理→写 SharedDetections）
    //
    // EncodeThread 每帧从 SharedDetections 读最新检测结果叠框，与推理速度解耦。
    // InferThread 会丢弃 infer_queue 中的旧帧，始终处理最新帧。
    //
    SharedDetections shared_dets; // 推理结果共享区，InferThread 写、EncodeThread 读

    // enc_queue 容量=1：约 68ms 缓冲，背压控制采集速度
    BlockingQueue<Frame>         enc_queue(1);
    // infer_queue 容量=2：CaptureThread 非阻塞投递，InferThread 忙时直接丢帧
    BlockingQueue<Frame>         infer_queue(2);
    // stream_queue 容量=2：约 137ms 缓冲，平滑编码抖动
    BlockingQueue<EncodedPacket> stream_queue(2);
    // mqtt_queue 容量=16：检测摘要异步上报，避免 MQTT 网络延迟阻塞推理
    BlockingQueue<std::string>   mqtt_queue(16);

    // 各线程通过队列引用连接，无共享状态（除 shared_dets 外）
    std::unique_ptr<IVideoSource> video_source;
    if (source_type == "pull_stream") {
        video_source = std::make_unique<PullStreamThread>(pull_cfg, enc_queue, infer_queue);
    } else {
        video_source = std::make_unique<CaptureThread>(cam_cfg, enc_queue, infer_queue);
    }

    InferThread   infer   (model_cfg, det_cfg, image_cfg,      infer_queue, mqtt_queue, shared_dets);
    EncodeThread  encoder (enc_cfg,   enc_queue,    stream_queue, shared_dets);
    StreamThread  streamer(stream_cfg, stream_queue);
    MqttThread    mqtter  (mqtt_cfg,  mqtt_queue);

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    // 启动顺序：下游先于上游，避免上游投递时下游尚未就绪
    try {
        mqtter.start();
        streamer.start();
        encoder.start();
        infer.start();
        video_source->start();
    } catch (const std::exception& e) {
        std::cerr << "Startup failed: " << e.what() << "\n";
        return 1;
    }

    std::cout << "AlertGateway running — press Ctrl+C to stop\n" << std::flush;
    while (g_running)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "\nShutting down...\n";

    // 关闭顺序：先停采集，再关队列让下游线程感知到 EOF 后自然退出
    video_source->stop();
    enc_queue.close();
    infer_queue.close();

    encoder.stop();
    infer.stop();
    stream_queue.close();

    streamer.stop();
    mqtt_queue.close();
    mqtter.stop();

    std::cout << "Done.\n";
    return 0;
}
