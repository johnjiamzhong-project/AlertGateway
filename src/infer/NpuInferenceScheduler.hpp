#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/Frame.hpp"

// 进程级 NPU 调度配置。P1 只实现调度算法；实际 RKNN executor 在 P2 实现。
struct NpuSchedulerConfig {
    bool enabled = false;
    std::string mode = "global_serial";
    std::string core_mask = "0_1_2";
    int global_target_fps = 0;
    int max_frame_age_ms = 250;
    int stats_interval_sec = 10;
};

struct NpuChannelConfig {
    std::string channel_id;
    int target_infer_fps = 0;
    int weight = 1;
};

// P1 的 executor 返回值；P2 会补充检测结果及预处理/RKNN/后处理分段耗时。
struct NpuInferenceResult {
    std::string channel_id;
    uint64_t frame_id = 0;
    bool success = true;
    int frame_width = 0;
    int frame_height = 0;
    int64_t pts_ms = 0;
    int64_t timestamp_ms = 0;
    std::vector<Detection> detections;
    float preprocess_ms = 0.0f;
    float input_sync_ms = 0.0f;
    float npu_ms = 0.0f;
    float output_postprocess_ms = 0.0f;
};

struct NpuChannelStats {
    std::string channel_id;
    uint64_t received = 0;
    uint64_t mailbox_replaced = 0;
    uint64_t stale_dropped = 0;
    uint64_t throttled = 0;
    uint64_t dispatched = 0;
    uint64_t completed = 0;
    uint64_t failed = 0;
    float actual_fps = 0.0f;
    float npu_avg_ms = 0.0f;
    float npu_p95_ms = 0.0f;
    float queue_wait_avg_ms = 0.0f;
    float queue_wait_p95_ms = 0.0f;
    float e2e_p95_ms = 0.0f;
};

struct NpuSchedulerStats {
    uint64_t mailbox_replaced = 0;
    uint64_t stale_dropped = 0;
    uint64_t dispatched = 0;
    uint64_t completed = 0;
    uint64_t failed = 0;
    float npu_avg_ms = 0.0f;
    float npu_p95_ms = 0.0f;
    float queue_wait_avg_ms = 0.0f;
    float queue_wait_p95_ms = 0.0f;
    float e2e_p95_ms = 0.0f;
    float busy_ratio = 0.0f;
    std::vector<NpuChannelStats> channels;
};

class NpuInferenceScheduler {
public:
    using Completion = std::function<void(const NpuInferenceResult&)>;
    // P2 由 RKNN executor 替换该函数对象；P1 smoke 使用固定耗时假 executor。
    using Executor = std::function<NpuInferenceResult(const NpuChannelConfig&, const Frame&)>;

    explicit NpuInferenceScheduler(NpuSchedulerConfig config);
    ~NpuInferenceScheduler();

    NpuInferenceScheduler(const NpuInferenceScheduler&) = delete;
    NpuInferenceScheduler& operator=(const NpuInferenceScheduler&) = delete;

    const NpuSchedulerConfig& config() const { return config_; }
    void set_executor(Executor executor);
    void register_channel(NpuChannelConfig channel, Completion completion = {});
    void unregister_channel(const std::string& channel_id);
    bool submit(const std::string& channel_id, Frame frame);
    void start();
    void stop();
    bool is_running() const;
    NpuSchedulerStats stats() const;

private:
    struct ChannelState;
    struct PendingJob;

    void run();
    bool select_job_locked(PendingJob* job,
                           std::chrono::steady_clock::time_point* wake_at);
    void drop_stale_locked(std::chrono::steady_clock::time_point now);
    void emit_stats_if_due_locked(std::chrono::steady_clock::time_point now);

    NpuSchedulerConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::thread worker_;
    Executor executor_;
    std::map<std::string, ChannelState> channels_;
    std::vector<std::string> channel_order_;
    size_t next_round_robin_index_ = 0;
    bool accepting_ = false;
    bool stop_requested_ = false;
    bool worker_running_ = false;
    NpuSchedulerStats stats_;
    std::chrono::steady_clock::time_point stats_window_started_at_{};
    std::vector<float> npu_samples_;
    std::vector<float> queue_wait_samples_;
    std::vector<float> e2e_samples_;
    float busy_ms_ = 0.0f;
};

using NpuInferenceSchedulerPtr = std::shared_ptr<NpuInferenceScheduler>;
