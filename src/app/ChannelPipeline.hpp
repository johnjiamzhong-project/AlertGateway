#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "capture/IVideoSource.hpp"
#include "common/BlockingQueue.hpp"
#include "common/Frame.hpp"
#include "common/FrameBufferPool.hpp"
#include "common/SharedDetections.hpp"
#include "encode/EncodeThread.hpp"
#include "infer/InferThread.hpp"
#include "mqtt/MqttThread.hpp"
#include "stream/StreamThread.hpp"

// 单个输入通道的完整运行时对象。各通道的队列、跟踪状态和硬件上下文互不共享；
// main 只负责创建、启动和停止多个 ChannelPipeline。
class ChannelPipeline {
public:
    ChannelPipeline(std::string channel_id, nlohmann::json config);
    ~ChannelPipeline();

    ChannelPipeline(const ChannelPipeline&) = delete;
    ChannelPipeline& operator=(const ChannelPipeline&) = delete;

    const std::string& id() const { return channel_id_; }
    void start();
    void stop_source();
    void stop();

private:
    void build();

    std::string channel_id_;
    nlohmann::json config_;

    // 声明在队列之前，保证队列残留 Frame 释放时池仍然存活。
    std::shared_ptr<FrameBufferPool> frame_pool_;
    std::unique_ptr<BlockingQueue<Frame>> enc_queue_;
    std::unique_ptr<BlockingQueue<Frame>> infer_queue_;
    std::unique_ptr<BlockingQueue<EncodedPacket>> stream_queue_;
    std::unique_ptr<BlockingQueue<std::string>> mqtt_queue_;
    std::unique_ptr<SharedDetections> shared_dets_;
    std::unique_ptr<IVideoSource> video_source_;
    std::unique_ptr<InferThread> infer_;
    std::unique_ptr<EncodeThread> encoder_;
    std::unique_ptr<StreamThread> streamer_;
    std::unique_ptr<MqttThread> mqtter_;

    bool source_started_ = false;
    bool stopped_ = false;
};
