#pragma once

#include <memory>
#include <string>

#include "common/Frame.hpp"
#include "infer/NpuInferenceScheduler.hpp"

struct ModelConfig;

// RKNN 的唯一资源拥有者。P2 将模型、输入 DMA 内存和后处理器都收敛在此对象；
// P3 会让 NpuInferenceScheduler 的唯一 worker 独占一个该对象。
class RknnNpuExecutor {
public:
    explicit RknnNpuExecutor(const ModelConfig& config);
    ~RknnNpuExecutor();

    RknnNpuExecutor(const RknnNpuExecutor&) = delete;
    RknnNpuExecutor& operator=(const RknnNpuExecutor&) = delete;

    bool start(std::string* error = nullptr);
    void stop();
    bool ready() const;
    NpuInferenceResult execute(const NpuChannelConfig& channel, const Frame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
