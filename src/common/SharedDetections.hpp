#pragma once
#include <mutex>
#include <vector>
#include "common/Frame.hpp"

// 推理结果的线程间共享区，解耦推理速度与编码速度。
//
// 问题背景：NPU 推理一帧约 150ms，摄像头出帧约 67ms（15fps）。
// 若把检测结果随帧流走 BlockingQueue，编码帧率会被推理速度拖死。
//
// 解决方案：InferThread 每次推理完调 set() 更新结果；
// EncodeThread 每帧编码前调 get() 取最新结果叠框，无需等待推理完成。
// 代价是部分帧叠的是"上一次"检测框，但在 15fps 场景下视觉上无感知。
struct SharedDetections {
    mutable std::mutex mutex;
    std::vector<Detection> detections;

    // 由 InferThread 调用，覆盖上一次推理结果
    void set(std::vector<Detection> dets) {
        std::lock_guard<std::mutex> lk(mutex);
        detections = std::move(dets);
    }

    // 由 EncodeThread 调用，返回当前结果的副本（拷贝出来后可安全使用，不受下次 set() 影响）
    std::vector<Detection> get() const {
        std::lock_guard<std::mutex> lk(mutex);
        return detections;
    }
};
