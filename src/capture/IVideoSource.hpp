#pragma once

// 视频源抽象接口，由 CaptureThread（V4L2）和 PullStreamThread（RTMP/RTSP 拉流）共同实现。
// main.cpp 根据 source.type 配置项实例化对应实现；其余模块（InferThread、EncodeThread）
// 不感知具体来源，只消费 BlockingQueue<Frame>。
class IVideoSource {
public:
    virtual ~IVideoSource() = default;

    // 打开设备/连接流、启动内部线程，失败时抛出 std::runtime_error
    virtual void start() = 0;

    // 停止采集、关闭连接、join 内部线程；可多次调用，幂等
    virtual void stop() = 0;
};
