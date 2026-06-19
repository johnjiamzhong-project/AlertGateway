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
#include "infer/YoloPostprocess.hpp"

struct ModelConfig {
    std::string path;
    float conf_threshold;
    float iou_threshold;
    std::vector<std::string> target_classes;
    int infer_every_n_frames = 1;
};

struct DetectionConfig {
    int report_interval_sec = 1;
};

class InferThread {
public:
    InferThread(const ModelConfig& model_cfg,
                const DetectionConfig& det_cfg,
                BlockingQueue<Frame>& in_queue,
                BlockingQueue<std::string>& mqtt_queue,
                SharedDetections& shared_dets);
    ~InferThread();

    void start();
    void stop();

private:
    void run();
    bool load_model();
    std::string summarize(const std::vector<Detection>& dets);

    ModelConfig           cfg_;
    DetectionConfig       det_cfg_;
    BlockingQueue<Frame>& in_queue_;
    BlockingQueue<std::string>& mqtt_queue_;
    SharedDetections&     shared_dets_;
    std::thread           thread_;
    std::atomic<bool>     running_{false};

    rknn_context                     ctx_       = 0;
    int                              n_output_  = 0;
    int                              box_output_idx_ = 0;
    int                              cls_output_idx_ = 1;
    std::unique_ptr<YoloPostprocess> postprocess_;

    // Zero-copy input buffer (pre-allocated NPU DMA memory)
    rknn_tensor_mem*   input_mem_           = nullptr;

    std::string                             last_summary_;
    std::chrono::steady_clock::time_point   last_report_time_;
};
