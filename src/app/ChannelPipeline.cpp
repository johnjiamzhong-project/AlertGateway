#include "app/ChannelPipeline.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

#include "capture/CaptureThread.hpp"
#include "capture/PullStreamThread.hpp"

using json = nlohmann::json;

namespace {

constexpr const char* kSingleResultRtmp = "rtmp://192.168.0.168/live/alertgateway";
constexpr const char* kMultiResultRtmpPrefix = "rtmp://192.168.0.168/live/alertgateway_channel_";

StreamSinkType parse_sink(const json& stream) {
    const std::string value = stream.value("sink", std::string("fixed_rtmp"));
    if (value == "fixed_rtmp") return StreamSinkType::FixedRtmp;
    if (value == "local_flv") return StreamSinkType::LocalFlv;
    if (value == "metrics_only") return StreamSinkType::MetricsOnly;
    throw std::runtime_error("Config error: stream.sink must be fixed_rtmp, local_flv or metrics_only");
}

TrackerConfig tracker_config_from(const ModelConfig& model) {
    return {
        model.track_display_mode,
        model.track_confirm_hits,
        model.track_ttl_ms,
        model.track_match_iou,
        model.track_center_distance_ratio,
        model.track_ema_alpha,
        model.track_deadzone_center_px,
        model.track_deadzone_size_ratio,
        model.track_innovation_iou,
        model.track_max_correction_px,
        model.track_jump_confirm_hits,
        model.track_align_to_video_pts,
        model.track_adaptive_filter,
        model.track_center_alpha_min,
        model.track_center_alpha_max,
        model.track_size_alpha_min,
        model.track_size_alpha_max,
        model.track_center_gated_size_filter,
        model.track_low_motion_size_alpha_max,
        model.track_motion_full_response_ratio,
        model.track_motion_smoothing_alpha,
        model.track_display_hold_ms,
        model.track_debug_logging,
        model.track_reversal_damping_enabled,
        model.track_reversal_center_alpha_max,
        model.track_reversal_min_motion_ratio,
        model.track_motion_stats_logging,
        model.track_global_motion_center_filter,
        model.track_global_motion_smoothing_alpha,
        model.track_global_motion_min_tracks,
    };
}

void validate_model_config(const ModelConfig& model) {
    if (model.track_display_mode < 0 || model.track_display_mode > 1) {
        throw std::runtime_error("Config error: model.track_display_mode must be 0 (raw) or 1 (adaptive)");
    }
    if (model.track_reversal_center_alpha_max < 0.0f ||
        model.track_reversal_center_alpha_max > 1.0f ||
        model.track_reversal_min_motion_ratio < 0.0f) {
        throw std::runtime_error("Config error: invalid model.track_reversal_*");
    }
    if (model.track_center_gated_size_filter &&
        (model.track_low_motion_size_alpha_max < model.track_size_alpha_min ||
         model.track_low_motion_size_alpha_max > model.track_size_alpha_max)) {
        throw std::runtime_error(
            "Config error: model.track_low_motion_size_alpha_max must be within track_size alpha range");
    }
    if (model.track_global_motion_smoothing_alpha < 0.0f ||
        model.track_global_motion_smoothing_alpha > 1.0f ||
        model.track_global_motion_min_tracks < 2) {
        throw std::runtime_error("Config error: invalid model.track_global_motion_*");
    }
}

}  // namespace

ChannelPipeline::ChannelPipeline(std::string channel_id, json config)
    : channel_id_(std::move(channel_id)), config_(std::move(config)) {
    build();
}

ChannelPipeline::~ChannelPipeline() {
    stop();
}

void ChannelPipeline::build() {
    if (channel_id_.empty()) {
        throw std::runtime_error("Config error: channel id must not be empty");
    }

    std::string source_type = "v4l2";
    int source_width = 640;
    int source_height = 480;
    int source_fps = 15;
    CameraConfig camera_cfg{};
    PullStreamConfig pull_cfg{};

    if (config_.contains("source")) {
        const auto& source = config_.at("source");
        source_type = source.value("type", std::string("v4l2"));
        source_width = source.at("width").get<int>();
        source_height = source.at("height").get<int>();
        source_fps = source.at("fps").get<int>();
        if (source_type != "v4l2" && source_type != "pull_stream") {
            throw std::runtime_error("Config error: source.type must be v4l2 or pull_stream");
        }
        if (source_width <= 0 || source_height <= 0 || source_fps <= 0 ||
            (source_width & 1) || (source_height & 1)) {
            throw std::runtime_error("Config error: source width/height must be positive even values and fps positive");
        }
        if (source_type == "pull_stream") {
            pull_cfg.channel_id = channel_id_;
            pull_cfg.url = source.value("url", std::string{});
            if (pull_cfg.url.empty()) {
                throw std::runtime_error("Config error: source.url is required for pull_stream");
            }
            pull_cfg.width = source_width;
            pull_cfg.height = source_height;
            pull_cfg.fps = source_fps;
            pull_cfg.reconnect_sec = source.value("reconnect_sec", 5);
        } else {
            camera_cfg.channel_id = channel_id_;
            camera_cfg.device = source.at("device").get<std::string>();
            camera_cfg.width = source_width;
            camera_cfg.height = source_height;
            camera_cfg.fps = source_fps;
        }
    } else if (config_.contains("camera")) {
        const auto& camera = config_.at("camera");
        camera_cfg.channel_id = channel_id_;
        camera_cfg.device = camera.at("device").get<std::string>();
        camera_cfg.width = camera.at("width").get<int>();
        camera_cfg.height = camera.at("height").get<int>();
        camera_cfg.fps = camera.at("fps").get<int>();
        source_width = camera_cfg.width;
        source_height = camera_cfg.height;
        source_fps = camera_cfg.fps;
    } else {
        throw std::runtime_error("Config error: no source or camera node found");
    }

    const auto& model = config_.at("model");
    const auto& detection = config_.at("detection");
    ModelConfig model_cfg{};
    model_cfg.channel_id = channel_id_;
    model_cfg.path = model.at("path").get<std::string>();
    model_cfg.conf_threshold = model.at("conf_threshold").get<float>();
    model_cfg.iou_threshold = model.at("iou_threshold").get<float>();
    model_cfg.target_classes = detection.at("target_classes").get<std::vector<std::string>>();
    model_cfg.infer_every_n_frames = model.value("infer_every_n_frames", 1);
    model_cfg.output_layout = model.value("output_layout", std::string("decoded"));
    model_cfg.track_confirm_hits = model.value("track_confirm_hits", 2);
    model_cfg.track_ttl_ms = model.value("track_ttl_ms", 300);
    model_cfg.track_match_iou = model.value("track_match_iou", 0.20f);
    model_cfg.track_center_distance_ratio = model.value("track_center_distance_ratio", 0.12f);
    model_cfg.track_display_mode = model.value("track_display_mode", 1);
    model_cfg.track_ema_alpha = model.value("track_ema_alpha", 0.25f);
    model_cfg.track_deadzone_center_px = model.value("track_deadzone_center_px", 6.0f);
    model_cfg.track_deadzone_size_ratio = model.value("track_deadzone_size_ratio", 0.01f);
    model_cfg.track_innovation_iou = model.value("track_innovation_iou", 0.45f);
    model_cfg.track_max_correction_px = model.value("track_max_correction_px", 120.0f);
    model_cfg.track_jump_confirm_hits = model.value("track_jump_confirm_hits", 2);
    model_cfg.track_align_to_video_pts = model.value("track_align_to_video_pts", true);
    model_cfg.track_adaptive_filter = model.value("track_adaptive_filter", true);
    model_cfg.track_center_alpha_min = model.value("track_center_alpha_min", 0.18f);
    model_cfg.track_center_alpha_max = model.value("track_center_alpha_max", 0.90f);
    model_cfg.track_size_alpha_min = model.value("track_size_alpha_min", 0.12f);
    model_cfg.track_size_alpha_max = model.value("track_size_alpha_max", 0.45f);
    model_cfg.track_center_gated_size_filter = model.value("track_center_gated_size_filter", false);
    model_cfg.track_low_motion_size_alpha_max = model.value("track_low_motion_size_alpha_max", 0.20f);
    model_cfg.track_motion_full_response_ratio = model.value("track_motion_full_response_ratio", 1.20f);
    model_cfg.track_motion_smoothing_alpha = model.value("track_motion_smoothing_alpha", 0.35f);
    model_cfg.track_display_hold_ms = model.value("track_display_hold_ms", 100);
    model_cfg.track_debug_logging = model.value("track_debug_logging", false);
    model_cfg.track_reversal_damping_enabled = model.value("track_reversal_damping_enabled", false);
    model_cfg.track_reversal_center_alpha_max = model.value("track_reversal_center_alpha_max", 0.35f);
    model_cfg.track_reversal_min_motion_ratio = model.value("track_reversal_min_motion_ratio", 0.005f);
    model_cfg.track_motion_stats_logging = model.value("track_motion_stats_logging", false);
    model_cfg.track_global_motion_center_filter = model.value("track_global_motion_center_filter", false);
    model_cfg.track_global_motion_smoothing_alpha = model.value("track_global_motion_smoothing_alpha", 0.25f);
    model_cfg.track_global_motion_min_tracks = model.value("track_global_motion_min_tracks", 3);
    validate_model_config(model_cfg);

    DetectionConfig detection_cfg{};
    detection_cfg.report_interval_sec = detection.at("report_interval_sec").get<int>();

    ImageProcessingConfig image_cfg{};
    const json image_processing = config_.value("image_processing", json::object());
    if (image_processing.contains("thumbnail")) {
        const auto& thumbnail = image_processing.at("thumbnail");
        image_cfg.thumbnail.enabled = thumbnail.value("enabled", false);
        image_cfg.thumbnail.width = thumbnail.value("width", 320);
        image_cfg.thumbnail.height = thumbnail.value("height", 180);
        image_cfg.thumbnail.on_detection_only = thumbnail.value("on_detection_only", true);
    }
    if (image_processing.contains("roi")) {
        const auto& roi = image_processing.at("roi");
        image_cfg.roi.enabled = roi.value("enabled", false);
        image_cfg.roi.filter_outside = roi.value("filter_outside", true);
        image_cfg.roi.track_dwell_sec = roi.value("track_dwell_sec", 0.0f);
        for (const auto& item : roi.value("regions", json::array())) {
            image_cfg.roi.regions.push_back({
                item.value("id", std::string("roi-") + std::to_string(image_cfg.roi.regions.size())),
                item.value("x", 0.0f), item.value("y", 0.0f),
                item.value("w", 1.0f), item.value("h", 1.0f),
            });
        }
    }
    if (image_processing.contains("tiling")) {
        const auto& tiling = image_processing.at("tiling");
        image_cfg.tiling.enabled = tiling.value("enabled", false);
        image_cfg.tiling.grid_cols = tiling.value("grid_cols", 2);
        image_cfg.tiling.grid_rows = tiling.value("grid_rows", 1);
        image_cfg.tiling.overlap_ratio = tiling.value("overlap_ratio", 0.10f);
        image_cfg.tiling.merge_iou_threshold = tiling.value("merge_iou_threshold", 0.45f);
    }

    const json stream = config_.value("stream", json::object());
    const StreamSinkType sink = parse_sink(stream);
    std::string output_target;
    if (sink == StreamSinkType::FixedRtmp) {
        output_target = stream.value("rtmp_url", std::string{});
        const std::string expected = channel_id_ == "single"
            ? kSingleResultRtmp
            : std::string(kMultiResultRtmpPrefix) + channel_id_;
        if (output_target != expected) {
            throw std::runtime_error("Config error: fixed_rtmp must use " + expected);
        }
    } else if (sink == StreamSinkType::LocalFlv) {
        output_target = stream.value("path", stream.value("rtmp_url", std::string{}));
        if (output_target.empty()) {
            throw std::runtime_error("Config error: local_flv requires stream.path");
        }
    }
    EncodeConfig encode_cfg{};
    encode_cfg.channel_id = channel_id_;
    encode_cfg.width = source_width;
    encode_cfg.height = source_height;
    encode_cfg.fps = source_fps;
    encode_cfg.bitrate_kbps = stream.value("bitrate_kbps", 2000);
    encode_cfg.draw_detection_labels = stream.value("draw_detection_labels", true);
    encode_cfg.detection_alignment_delay_frames = stream.value("detection_alignment_delay_frames", 2);
    encode_cfg.render_overlap_audit = stream.value("render_overlap_audit", true);
    encode_cfg.render_overlap_min_intersection_area_px =
        stream.value("render_overlap_min_intersection_area_px", 1.0f);
    if (encode_cfg.render_overlap_min_intersection_area_px < 0.0f) {
        throw std::runtime_error("Config error: stream.render_overlap_min_intersection_area_px must be non-negative");
    }
    StreamConfig stream_cfg{};
    stream_cfg.channel_id = channel_id_;
    stream_cfg.sink = sink;
    stream_cfg.rtmp_url = output_target;
    stream_cfg.width = source_width;
    stream_cfg.height = source_height;
    stream_cfg.fps = source_fps;

    const auto& mqtt = config_.at("mqtt");
    MqttConfig mqtt_cfg{};
    mqtt_cfg.channel_id = channel_id_;
    mqtt_cfg.broker = mqtt.at("broker").get<std::string>();
    mqtt_cfg.port = mqtt.at("port").get<int>();
    mqtt_cfg.topic = mqtt.at("topic").get<std::string>();
    mqtt_cfg.client_id = mqtt.at("client_id").get<std::string>();

    enc_queue_ = std::make_unique<BlockingQueue<Frame>>(1);
    infer_queue_ = std::make_unique<BlockingQueue<Frame>>(1);
    stream_queue_ = std::make_unique<BlockingQueue<EncodedPacket>>(2);
    mqtt_queue_ = std::make_unique<BlockingQueue<std::string>>(16);
    frame_pool_ = std::make_shared<FrameBufferPool>(4);
    shared_dets_ = std::make_unique<SharedDetections>(tracker_config_from(model_cfg));
    if (source_type == "pull_stream") {
        video_source_ = std::make_unique<PullStreamThread>(pull_cfg, *enc_queue_, *infer_queue_, frame_pool_);
    } else {
        video_source_ = std::make_unique<CaptureThread>(camera_cfg, *enc_queue_, *infer_queue_, frame_pool_);
    }
    infer_ = std::make_unique<InferThread>(model_cfg, detection_cfg, image_cfg,
                                           *infer_queue_, *mqtt_queue_, *shared_dets_);
    encoder_ = std::make_unique<EncodeThread>(encode_cfg, *enc_queue_, *stream_queue_, *shared_dets_);
    streamer_ = std::make_unique<StreamThread>(stream_cfg, *stream_queue_);
    mqtter_ = std::make_unique<MqttThread>(mqtt_cfg, *mqtt_queue_);
}

void ChannelPipeline::start() {
    if (stopped_) {
        throw std::runtime_error("ChannelPipeline cannot be restarted after stop: " + channel_id_);
    }
    try {
        std::cout << "[ChannelPipeline] starting channel=" << channel_id_ << "\n" << std::flush;
        mqtter_->start();
        streamer_->start();
        encoder_->start();
        infer_->start();
        video_source_->start();
        source_started_ = true;
    } catch (...) {
        stop();
        throw;
    }
}

void ChannelPipeline::stop_source() {
    if (!source_started_) return;
    video_source_->stop();
    source_started_ = false;
}

void ChannelPipeline::stop() {
    if (stopped_) return;
    stop_source();
    stopped_ = true;
    if (enc_queue_) enc_queue_->close();
    if (infer_queue_) infer_queue_->close();
    if (encoder_) encoder_->stop();
    if (infer_) infer_->stop();
    if (stream_queue_) stream_queue_->close();
    if (streamer_) streamer_->stop();
    if (mqtt_queue_) mqtt_queue_->close();
    if (mqtter_) mqtter_->stop();
    std::cout << "[ChannelPipeline] stopped channel=" << channel_id_ << "\n" << std::flush;
}
