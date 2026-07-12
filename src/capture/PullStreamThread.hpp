#pragma once
#include <string>
#include <thread>
#include <atomic>
#include "common/BlockingQueue.hpp"
#include "common/Frame.hpp"
#include "capture/IVideoSource.hpp"

// 拉流配置，从 config.json 读取后传入 PullStreamThread
struct PullStreamConfig {
    std::string url;          // RTSP/RTMP/HTTP-FLV 拉流地址，如 rtmp://127.0.0.1/live/desk
    int width;                // 期望分辨率宽（像素）
    int height;               // 期望分辨率高（像素）
    int fps;                  // 期望帧率
    int reconnect_sec = 5;    // 断线重连等待秒数，默认 5 秒
};

// PullStreamThread：实现 IVideoSource 接口的拉流线程。
//
// 使用 FFmpeg 从 SRS 等服务器拉取 H.264/H.265 码流，在本地软解码为 YUV 图像，
// 然后进行 NV12 格式转置后填入队列，驱动下游 InferThread 与 EncodeThread。
//
// 队列投递规则与 CaptureThread 一致：
//   enc_queue  — 阻塞投递，平滑背压，避免漏帧
//   infer_queue — 非阻塞投递（timeout=0），当推理线程繁忙时直接丢弃，始终推理最新帧
class PullStreamThread : public IVideoSource {
public:
    PullStreamThread(const PullStreamConfig& cfg,
                     BlockingQueue<Frame>& enc_queue,
                     BlockingQueue<Frame>& infer_queue);
    ~PullStreamThread();

    // 启动拉流工作线程
    void start() override;
    // 停止拉流并等待工作线程 join 退出，释放 FFmpeg 资源
    void stop() override;

private:
    static int interrupt_callback(void* opaque);
    void run();          // 工作线程主循环

    PullStreamConfig        cfg_;
    BlockingQueue<Frame>&   enc_queue_;    // 投递给 EncodeThread
    BlockingQueue<Frame>&   infer_queue_;  // 投递给 InferThread（丢帧）
    std::thread             thread_;
    std::atomic<bool>       running_{false};
};
