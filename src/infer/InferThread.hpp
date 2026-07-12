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
    std::string path;
    float conf_threshold;
    float iou_threshold;
    std::vector<std::string> target_classes;
    int infer_every_n_frames = 1;
    std::string output_layout = "decoded";
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
    std::vector<Detection> infer_once(const Frame& frame);
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
