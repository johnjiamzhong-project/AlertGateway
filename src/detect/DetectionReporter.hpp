#pragma once
#include <thread>
#include <atomic>
#include <string>
#include <chrono>
#include "common/BlockingQueue.hpp"
#include "common/Frame.hpp"

struct DetectionConfig {
    int report_interval_sec = 1;
};

class DetectionReporter {
public:
    DetectionReporter(const DetectionConfig& cfg,
                      BlockingQueue<Frame>& in_queue,
                      BlockingQueue<Frame>& encode_queue,
                      BlockingQueue<std::string>& mqtt_queue);
    ~DetectionReporter();

    void start();
    void stop();

private:
    void run();
    void draw_detections(Frame& frame);
    std::string summarize(const std::vector<Detection>& dets);

    DetectionConfig             cfg_;
    BlockingQueue<Frame>&       in_queue_;
    BlockingQueue<Frame>&       encode_queue_;
    BlockingQueue<std::string>& mqtt_queue_;
    std::thread                 thread_;
    std::atomic<bool>           running_{false};

    std::string                                last_summary_;
    std::chrono::steady_clock::time_point      last_report_time_;
};
