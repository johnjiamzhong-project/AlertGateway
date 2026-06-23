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
    std::vector<std::string> target_classes;  // 空=不过滤，只保留这些类别的检测框
    int infer_every_n_frames = 1;             // 每N帧推理一次，NPU跟不上摄像头帧率时用这个降频
};

struct DetectionConfig {
    int report_interval_sec = 1;  // MQTT上报的最小间隔；配合 last_summary_ 做"变化时才发"去重
};

// NPU 推理线程：取帧 → 格式转换 → RKNN 推理 → YOLO 后处理 → 写共享结果/MQTT。
class InferThread {
public:
    InferThread(const ModelConfig& model_cfg,
                const DetectionConfig& det_cfg,
                BlockingQueue<Frame>& in_queue,
                BlockingQueue<std::string>& mqtt_queue,
                SharedDetections& shared_dets);
    ~InferThread();

    void start();  // 加载模型并启动推理线程
    void stop();   // 停线程并释放RKNN资源

private:
    void run();          // 推理主循环（取帧→转换→NPU推理→后处理→写结果/MQTT），详见 .cpp
    bool load_model();   // 加载模型、配置NPU、绑定zero-copy输入，详见 .cpp
    std::string summarize(const std::vector<Detection>& dets);  // 检测结果转JSON摘要，详见 .cpp

    ModelConfig           cfg_;
    DetectionConfig       det_cfg_;
    BlockingQueue<Frame>& in_queue_;
    BlockingQueue<std::string>& mqtt_queue_;
    SharedDetections&     shared_dets_;
    std::thread           thread_;
    std::atomic<bool>     running_{false};

    rknn_context                     ctx_       = 0;   // RKNN 运行时上下文
    int                              n_output_  = 0;    // 模型输出张量总数
    int                              box_output_idx_ = 0;  // box/cls 各自的输出index，load_model()里按元素数查询确定
    int                              cls_output_idx_ = 1;
    std::unique_ptr<YoloPostprocess> postprocess_;

    rknn_tensor_mem*   input_mem_           = nullptr;  // zero-copy 输入DMA内存，load_model()分配，stop()释放

    std::string                             last_summary_;      // 上次推送MQTT的摘要，用于去重
    std::chrono::steady_clock::time_point   last_report_time_;  // 上次推送MQTT的时间点
};
