#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include "common/Frame.hpp"

// 复用已完成消费的像素 vector，减少多路拉流/采集时的频繁堆分配。
// acquire() 不等待：池空时直接新建，保证采集线程不会因缓冲池耗尽而阻塞。
class FrameBufferPool : public std::enable_shared_from_this<FrameBufferPool> {
public:
    explicit FrameBufferPool(size_t max_cached = 4) : max_cached_(max_cached) {}

    SharedByteBuffer acquire(size_t size) {
        std::unique_ptr<std::vector<uint8_t>> buffer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!free_.empty()) {
                buffer = std::move(free_.back());
                free_.pop_back();
            }
        }
        if (!buffer) buffer = std::make_unique<std::vector<uint8_t>>();
        if (buffer->size() != size) buffer->resize(size);

        auto* raw = buffer.release();
        std::weak_ptr<FrameBufferPool> weak_pool = shared_from_this();
        std::shared_ptr<std::vector<uint8_t>> shared(raw,
            [weak_pool](std::vector<uint8_t>* returned) {
                if (auto pool = weak_pool.lock()) pool->release(returned);
                else delete returned;
            });
        return SharedByteBuffer(std::move(shared));
    }

private:
    void release(std::vector<uint8_t>* buffer) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_.size() < max_cached_) free_.emplace_back(buffer);
        else delete buffer;
    }

    size_t max_cached_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> free_;
};
