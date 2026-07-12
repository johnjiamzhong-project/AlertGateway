#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include "common/BlockingQueue.hpp"
#include "common/Frame.hpp"
#include "capture/IVideoSource.hpp"


// 摄像头配置，从 config.json 读取后传入 CaptureThread
struct CameraConfig {
    std::string device;  // V4L2 设备节点，如 /dev/video20
    int width;           // 采集分辨率宽（像素）
    int height;          // 采集分辨率高（像素）
    int fps;             // 目标帧率，通过 VIDIOC_S_PARM 下发给驱动（驱动可能调整到最近支持值）
};

// V4L2 采集线程，Pipeline 的数据源头。实现 IVideoSource 接口。
//
// 使用 mmap 零拷贝模式：内核采集缓冲区直接映射到用户空间，
// 取帧时只做一次 assign 拷贝（内核buf → Frame::raw_data），
// 随即归还内核缓冲区（VIDIOC_QBUF），采集延迟最低。
//
// 每帧同时投递给两条下游：
//   enc_queue  — 阻塞投递（timeout=100ms），每帧必达，背压由队列容量控制
//   infer_queue — 非阻塞投递（timeout=0），InferThread 忙时直接丢帧，
//                 避免推理慢拖垮采集帧率
class CaptureThread : public IVideoSource {
public:
    CaptureThread(const CameraConfig& cfg,
                  BlockingQueue<Frame>& enc_queue,
                  BlockingQueue<Frame>& infer_queue);
    ~CaptureThread();

    // 依次执行：打开设备 → 申请 mmap 缓冲区 → 启动采集流 → 起线程
    void start();
    // 置停止标志 → join 线程 → 停止采集流 → 释放 mmap → 关闭 fd
    void stop();

private:
    void run();          // 主循环：select 等帧 → DQBUF → 投队列 → QBUF

    // V4L2 初始化三步，start() 中顺序调用
    bool open_device();  // open fd，VIDIOC_S_FMT 设置分辨率/YUYV格式，VIDIOC_S_PARM 设置帧率
    bool init_buffers(); // VIDIOC_REQBUFS 申请4块内核缓冲区，mmap 映射到用户空间
    bool start_stream(); // 将所有缓冲区 VIDIOC_QBUF 入队，VIDIOC_STREAMON 启动采集

    void stop_stream();  // VIDIOC_STREAMOFF 停止采集
    void close_device(); // munmap 释放所有缓冲区，close fd

    // V4L2 mmap 缓冲区描述符，内核分配、用户空间只读/写
    struct MmapBuffer {
        void*  start;   // mmap 映射地址
        size_t length;  // 缓冲区字节数
    };

    CameraConfig            cfg_;
    BlockingQueue<Frame>&   enc_queue_;    // 每帧必达 → EncodeThread
    BlockingQueue<Frame>&   infer_queue_;  // 尽力投递 → InferThread（可丢帧）
    std::thread             thread_;
    std::atomic<bool>       running_{false};
    int                     fd_ = -1;          // V4L2 设备文件描述符
    std::vector<MmapBuffer> buffers_;           // mmap 缓冲区列表（通常4块）
};
