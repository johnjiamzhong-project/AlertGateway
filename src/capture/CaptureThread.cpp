#include "capture/CaptureThread.hpp"
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <stdexcept>
#include <chrono>
#include <iostream>
#include <iomanip>

namespace {

std::string fourcc_to_string(uint32_t fourcc) {
    char value[5] = {
        static_cast<char>(fourcc & 0xff),
        static_cast<char>((fourcc >> 8) & 0xff),
        static_cast<char>((fourcc >> 16) & 0xff),
        static_cast<char>((fourcc >> 24) & 0xff),
        '\0'
    };
    return value;
}

double fps_from_timeperframe(const v4l2_fract& tpf) {
    if (tpf.numerator == 0 || tpf.denominator == 0) return 0.0;
    return static_cast<double>(tpf.denominator) / tpf.numerator;
}

}  // namespace

CaptureThread::CaptureThread(const CameraConfig& cfg,
                             BlockingQueue<Frame>& enc_queue,
                             BlockingQueue<Frame>& infer_queue,
                             std::shared_ptr<FrameBufferPool> frame_pool)
    : cfg_(cfg), enc_queue_(enc_queue), infer_queue_(infer_queue),
      frame_pool_(frame_pool ? std::move(frame_pool) : std::make_shared<FrameBufferPool>(4)) {}

CaptureThread::~CaptureThread() { stop(); }

void CaptureThread::start() {
    if (!open_device())  throw std::runtime_error("CaptureThread: failed to open " + cfg_.device);
    if (!init_buffers()) throw std::runtime_error("CaptureThread: failed to init V4L2 buffers");
    if (!start_stream()) throw std::runtime_error("CaptureThread: failed to start V4L2 stream");
    running_ = true;
    thread_ = std::thread(&CaptureThread::run, this);
}

void CaptureThread::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    stop_stream();
    close_device();
}

void CaptureThread::run() {
    uint64_t next_frame_id = 0;
    while (running_) {
        // select 阻塞等待摄像头帧就绪，超时 2s 后继续循环（检查 running_ 标志）
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd_, &fds);
        struct timeval tv{ .tv_sec = 2, .tv_usec = 0 };

        //只检查 `fd_` 是否进入可读状态
        int r = select(fd_ + 1, &fds, nullptr, nullptr, &tv);
        if (r == -1) { if (errno == EINTR) continue; break; }
        if (r == 0)  continue; // 超时，没有新帧

        // 从内核取出已采集的帧缓冲区
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) == -1) continue;

        Frame frame;
        frame.width  = cfg_.width;
        frame.height = cfg_.height;
        frame.frame_id = ++next_frame_id;
        frame.pts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        frame.timestamp_ms = frame.pts_ms;

        // 将 mmap 缓冲区数据拷贝到 Frame（YUYV422 格式，宽×高×2 字节）
        // 拷贝后立即归还缓冲区（VIDIOC_QBUF），内核可继续复用此缓冲区采集下一帧
        const auto* src = static_cast<const uint8_t*>(buffers_[buf.index].start);
        frame.raw_data = frame_pool_->acquire(buf.bytesused);
        frame.raw_data.assign(src, src + buf.bytesused);

        // 向 InferThread 投递帧副本，非阻塞（timeout=0）
        // InferThread 忙时 push 返回 false，帧被丢弃，不影响采集节奏
        Frame infer_copy = frame;
        infer_queue_.push_latest(std::move(infer_copy));

        // 向 EncodeThread 投递帧，阻塞等待至多 100ms
        // enc_queue 容量=1，此处是唯一的背压点：编码跟不上时采集会短暂等待
        enc_queue_.push(std::move(frame), 100);

        // 将缓冲区归还内核，供下一帧采集使用
        if (ioctl(fd_, VIDIOC_QBUF, &buf) == -1) break;
    }
}

bool CaptureThread::open_device() {
    // O_NONBLOCK：配合 select 使用，VIDIOC_DQBUF 不会阻塞
    fd_ = open(cfg_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ == -1) return false;

    // 设置采集格式：分辨率 + YUYV422 像素格式。
    // VIDIOC_S_FMT 是请求，驱动可能改写为实际支持的档位，所以设置后必须回读校验。
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = cfg_.width;
    fmt.fmt.pix.height      = cfg_.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;
    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) == -1) { close(fd_); fd_ = -1; return false; }

    struct v4l2_format actual_fmt;
    memset(&actual_fmt, 0, sizeof(actual_fmt));
    actual_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_G_FMT, &actual_fmt) == -1) {
        std::cerr << "VIDIOC_G_FMT failed after VIDIOC_S_FMT: "
                  << strerror(errno) << "\n";
        close(fd_); fd_ = -1; return false;
    }

    const auto& pix = actual_fmt.fmt.pix;
    std::cout << "V4L2 format negotiated: "
              << pix.width << "x" << pix.height
              << " fourcc=" << fourcc_to_string(pix.pixelformat)
              << " bytesperline=" << pix.bytesperline
              << " sizeimage=" << pix.sizeimage << "\n" << std::flush;

    if (pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        std::cerr << "Unsupported V4L2 pixel format: "
                  << fourcc_to_string(pix.pixelformat)
                  << " (expected YUYV)\n";
        close(fd_); fd_ = -1; return false;
    }
    if (static_cast<int>(pix.width) != cfg_.width ||
        static_cast<int>(pix.height) != cfg_.height) {
        std::cerr << "V4L2 resolution mismatch: requested "
                  << cfg_.width << "x" << cfg_.height
                  << ", got " << pix.width << "x" << pix.height << "\n";
        close(fd_); fd_ = -1; return false;
    }
    uint32_t expected_bytesperline = pix.width * 2;
    if (pix.bytesperline != 0 && pix.bytesperline != expected_bytesperline) {
        std::cerr << "Unsupported V4L2 bytesperline: got "
                  << pix.bytesperline << ", expected "
                  << expected_bytesperline << " for packed YUYV\n";
        close(fd_); fd_ = -1; return false;
    }

    // 设置帧率（同样是请求，驱动可能不支持时会改写或忽略）。
    // fps 必须与摄像头实际输出帧率尽量一致，否则 PTS 节奏错乱导致延迟异常（见 BUG-006）。
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = cfg_.fps;
    if (ioctl(fd_, VIDIOC_S_PARM, &parm) == -1) {
        std::cerr << "VIDIOC_S_PARM failed: " << strerror(errno)
                  << " (continuing with driver default fps)\n";
    }

    struct v4l2_streamparm actual_parm;
    memset(&actual_parm, 0, sizeof(actual_parm));
    actual_parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_G_PARM, &actual_parm) == -1) {
        std::cerr << "VIDIOC_G_PARM failed after VIDIOC_S_PARM: "
                  << strerror(errno) << "\n";
    } else {
        const auto& tpf = actual_parm.parm.capture.timeperframe;
        double actual_fps = fps_from_timeperframe(tpf);
        std::ios::fmtflags old_flags = std::cout.flags();
        std::streamsize old_precision = std::cout.precision();
        std::cout << "V4L2 fps negotiated: "
                  << tpf.denominator << "/" << tpf.numerator
                  << " (" << std::fixed << std::setprecision(3)
                  << actual_fps << " fps), requested "
                  << cfg_.fps << " fps\n" << std::flush;
        std::cout.flags(old_flags);
        std::cout.precision(old_precision);

        if (actual_fps > 0.0 && std::abs(actual_fps - cfg_.fps) > 0.5) {
            std::cerr << "V4L2 fps differs from config by more than 0.5 fps; "
                      << "encoded PTS still uses configured fps="
                      << cfg_.fps << "\n";
        }
    }

    return true;
}

bool CaptureThread::init_buffers() {
    // 向内核申请 4 块采集缓冲区（mmap 模式）
    // 4块缓冲区形成环形队列：内核填充 → 应用取出处理 → 归还内核，减少帧丢失
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = 4;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd_, VIDIOC_REQBUFS, &req) == -1) return false;

    buffers_.resize(req.count);
    for (size_t i = 0; i < req.count; i++) {
        // 查询每块缓冲区的物理偏移和大小
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) == -1) return false;

        // 将内核缓冲区映射到用户空间，后续直接读取无需系统调用拷贝
        buffers_[i].length = buf.length;
        buffers_[i].start  = mmap(nullptr, buf.length,
                                   PROT_READ | PROT_WRITE, MAP_SHARED,
                                   fd_, buf.m.offset);
        if (buffers_[i].start == MAP_FAILED) return false;
    }
    return true;
}

bool CaptureThread::start_stream() {
    // 将所有缓冲区入队（交给内核），内核按序填充帧数据
    for (size_t i = 0; i < buffers_.size(); i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (ioctl(fd_, VIDIOC_QBUF, &buf) == -1) return false;
    }
    // 启动采集流，之后内核开始持续往缓冲区填帧
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    return ioctl(fd_, VIDIOC_STREAMON, &type) != -1;
}

void CaptureThread::stop_stream() {
    if (fd_ == -1) return;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type);
}

void CaptureThread::close_device() {
    // 释放所有 mmap 映射，再关闭设备 fd
    for (auto& b : buffers_) {
        if (b.start && b.start != MAP_FAILED)
            munmap(b.start, b.length);
    }
    buffers_.clear();
    if (fd_ != -1) { close(fd_); fd_ = -1; }
}
