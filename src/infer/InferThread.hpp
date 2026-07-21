#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <vector>
#include <chrono>
#include "rknn_api.h"
#include "im2d.h"
#include "rga.h"
#include "common/BlockingQueue.hpp"
#include "common/Frame.hpp"
#include "common/SharedDetections.hpp"
#include "infer/RockchipYoloPostprocess.hpp"
#include "infer/YoloPostprocess.hpp"
#include "infer/ImageProcessing.hpp"
#include "infer/RoiFilter.hpp"
#include "infer/TilingTask.hpp"
#include "infer/ThumbnailTask.hpp"

struct ModelConfig {
    std::string channel_id = "single";
    std::string path;
    float conf_threshold;
    float iou_threshold;
    std::vector<std::string> target_classes;
    int infer_every_n_frames = 1;
    std::string output_layout = "decoded";
    int track_confirm_hits = 2;
    int track_ttl_ms = 300;
    float track_match_iou = 0.20f;
    float track_center_distance_ratio = 0.12f;
    int track_display_mode = 1;
    float track_ema_alpha = 0.25f;
    float track_deadzone_center_px = 6.0f;
    float track_deadzone_size_ratio = 0.01f;
    float track_innovation_iou = 0.45f;
    float track_max_correction_px = 120.0f;
    int track_jump_confirm_hits = 2;
    bool track_align_to_video_pts = true;
    bool track_adaptive_filter = true;
    float track_center_alpha_min = 0.18f;
    float track_center_alpha_max = 0.90f;
    float track_size_alpha_min = 0.12f;
    float track_size_alpha_max = 0.45f;
    bool track_center_gated_size_filter = false;
    float track_low_motion_size_alpha_max = 0.20f;
    float track_motion_full_response_ratio = 1.20f;
    float track_motion_smoothing_alpha = 0.35f;
    int track_display_hold_ms = 100;
    bool track_debug_logging = false;
    bool track_reversal_damping_enabled = false;
    float track_reversal_center_alpha_max = 0.35f;
    float track_reversal_min_motion_ratio = 0.005f;
    bool track_motion_stats_logging = false;
    bool track_global_motion_center_filter = false;
    float track_global_motion_smoothing_alpha = 0.25f;
    int track_global_motion_min_tracks = 3;
};
struct DetectionConfig {
    int report_interval_sec = 1;
};
class InferThread {
public:
    InferThread(const ModelConfig& model_cfg, const DetectionConfig& det_cfg,
                const ImageProcessingConfig& image_cfg,
                BlockingQueue<Frame>& in_queue,
                BlockingQueue<std::string>& mqtt_queue,
                SharedDetections& shared_dets);
    ~InferThread();
    void start();
    void stop();
private:
    void run();
    bool load_model();
    std::vector<Detection> infer_once(const Frame& frame, const TileRect* region = nullptr);
    std::string summarize(const std::vector<Detection>& dets,
                          const Thumbnail* thumbnail,
                          const std::vector<RoiEvent>* events);
    std::string summary_key(const std::vector<Detection>& dets);

    ModelConfig cfg_;
    DetectionConfig det_cfg_;
    ImageProcessingConfig image_cfg_;
    BlockingQueue<Frame>& in_queue_;
    BlockingQueue<std::string>& mqtt_queue_;
    SharedDetections& shared_dets_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    rknn_context ctx_ = 0;
    int n_output_ = 0;
    int box_output_idx_ = -1;
    int cls_output_idx_ = -1;
    int model_width_ = 0;
    int model_height_ = 0;
    std::vector<rknn_tensor_attr> output_attrs_;
    std::unique_ptr<YoloPostprocess> decoded_postprocess_;
    std::unique_ptr<RockchipYoloPostprocess> rockchip_postprocess_;
    rknn_tensor_mem* input_mem_ = nullptr;
    std::string last_summary_key_;
    std::chrono::steady_clock::time_point last_report_time_;
};
