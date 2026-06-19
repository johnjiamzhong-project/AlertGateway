#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>

// 有界阻塞队列，用于 Pipeline 各线程之间的帧/包传递。
//
// 容量上限由构造参数 max_size 决定，各队列的容量设计：
//   enc_queue(1)    — 采集→编码，容量=1 形成背压，防止采集过快堆积
//   infer_queue(2)  — 采集→推理，push 用 timeout=0（非阻塞），推理忙时直接丢帧
//   stream_queue(2) — 编码→推流，容量=2 平滑编码抖动（~137ms 缓冲）
//   mqtt_queue(16)  — 推理→MQTT，容量大避免 MQTT 网络延迟反压推理线程
//
// close() 后所有阻塞的 push/pop 立即返回 false，线程可据此退出循环。
template<typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t max_size) : max_size_(max_size), closed_(false) {}

    // 向队列投递一项。队列满时：
    //   timeout_ms < 0  → 永久阻塞等待空位
    //   timeout_ms = 0  → 立即返回 false（非阻塞，用于可丢帧场景）
    //   timeout_ms > 0  → 等待至多 timeout_ms 毫秒
    // 队列已关闭时始终返回 false。
    bool push(T item, int timeout_ms = -1) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto pred = [this] { return queue_.size() < max_size_ || closed_; };
        if (timeout_ms < 0) {
            not_full_.wait(lock, pred);
        } else {
            if (!not_full_.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred))
                return false;
        }
        if (closed_) return false;
        queue_.push(std::move(item));
        not_empty_.notify_one();
        return true;
    }

    // 从队列取出一项。队列空时：
    //   timeout_ms < 0  → 永久阻塞等待数据
    //   timeout_ms = 0  → 立即返回 false
    //   timeout_ms > 0  → 等待至多 timeout_ms 毫秒（各线程 run() 循环用 200ms，
    //                      超时后检查 running_ 标志，避免线程永久卡死）
    // 队列已关闭且为空时返回 false，线程可据此退出。
    bool pop(T& item, int timeout_ms = -1) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto pred = [this] { return !queue_.empty() || closed_; };
        if (timeout_ms < 0) {
            not_empty_.wait(lock, pred);
        } else {
            if (!not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred))
                return false;
        }
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop();
        not_full_.notify_one();
        return true;
    }

    // 关闭队列：唤醒所有阻塞的 push/pop，使其返回 false。
    // main() 在停止上游线程后调用，通知下游线程数据流已结束。
    void close() {
        std::unique_lock<std::mutex> lock(mutex_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool is_closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<T>           queue_;
    mutable std::mutex      mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    size_t max_size_;
    bool   closed_;
};
