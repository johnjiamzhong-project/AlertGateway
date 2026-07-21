#include "infer/NpuInferenceScheduler.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
}

Frame frame(uint64_t frame_id, int64_t timestamp_ms = now_ms()) {
    Frame value;
    value.frame_id = frame_id;
    value.timestamp_ms = timestamp_ms;
    value.pts_ms = timestamp_ms;
    return value;
}

template <typename Predicate>
bool wait_for(Predicate predicate, int timeout_ms = 2000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

const NpuChannelStats& stats_for(const NpuSchedulerStats& stats, const std::string& channel_id) {
    const auto it = std::find_if(stats.channels.begin(), stats.channels.end(),
                                 [&channel_id](const NpuChannelStats& value) {
                                     return value.channel_id == channel_id;
                                 });
    assert(it != stats.channels.end());
    return *it;
}

void verify_latest_frame_mailbox() {
    NpuInferenceScheduler scheduler({false, "global_serial", "0_1_2", 1000, 1000, 10});
    std::mutex mutex;
    std::vector<uint64_t> executed;
    scheduler.set_executor([&](const NpuChannelConfig&, const Frame& input) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            executed.push_back(input.frame_id);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        return NpuInferenceResult{};
    });
    scheduler.register_channel({"one", 1000, 1});
    scheduler.start();
    assert(scheduler.submit("one", frame(1)));
    assert(wait_for([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return !executed.empty();
    }));
    for (uint64_t id = 2; id <= 100; ++id) assert(scheduler.submit("one", frame(id)));
    assert(wait_for([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return executed.size() >= 2;
    }));
    scheduler.stop();

    std::lock_guard<std::mutex> lock(mutex);
    assert(executed.front() == 1);
    assert(executed.back() == 100);
    const auto snapshot = scheduler.stats();
    assert(stats_for(snapshot, "one").mailbox_replaced >= 98);
}

void verify_round_robin() {
    NpuInferenceScheduler scheduler({false, "global_serial", "0_1_2", 1000, 1000, 10});
    std::mutex mutex;
    std::vector<std::string> executed;
    scheduler.set_executor([&](const NpuChannelConfig& channel, const Frame&) {
        std::lock_guard<std::mutex> lock(mutex);
        executed.push_back(channel.channel_id);
        return NpuInferenceResult{};
    });
    for (const char* id : {"1", "2", "3", "4"}) scheduler.register_channel({id, 1000, 1});
    scheduler.start();
    for (const char* id : {"1", "2", "3", "4"}) assert(scheduler.submit(id, frame(1)));
    assert(wait_for([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return executed.size() == 4;
    }));
    scheduler.stop();

    std::lock_guard<std::mutex> lock(mutex);
    assert((executed == std::vector<std::string>{"1", "2", "3", "4"}));
}

void verify_rate_limit_and_stale_drop() {
    NpuInferenceScheduler scheduler({false, "global_serial", "0_1_2", 7, 1000, 10});
    std::mutex mutex;
    std::vector<Clock::time_point> dispatched_at;
    scheduler.set_executor([&](const NpuChannelConfig&, const Frame&) {
        std::lock_guard<std::mutex> lock(mutex);
        dispatched_at.push_back(Clock::now());
        return NpuInferenceResult{};
    });
    scheduler.register_channel({"one", 7, 1});
    scheduler.start();
    assert(scheduler.submit("one", frame(1, now_ms() - 2000)));
    assert(wait_for([&] { return scheduler.stats().stale_dropped == 1; }));
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(dispatched_at.empty());
    }

    assert(scheduler.submit("one", frame(2)));
    assert(wait_for([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return dispatched_at.size() == 1;
    }));
    assert(scheduler.submit("one", frame(3)));
    assert(wait_for([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return dispatched_at.size() == 2;
    }));
    scheduler.stop();

    std::lock_guard<std::mutex> lock(mutex);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        dispatched_at[1] - dispatched_at[0]).count();
    assert(elapsed >= 120);
}

void verify_stop_discards_pending_without_callback() {
    NpuInferenceScheduler scheduler({false, "global_serial", "0_1_2", 1000, 1000, 10});
    std::mutex mutex;
    int executor_calls = 0;
    int completion_calls = 0;
    scheduler.set_executor([&](const NpuChannelConfig&, const Frame&) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++executor_calls;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return NpuInferenceResult{};
    });
    scheduler.register_channel({"one", 1000, 1}, [&](const NpuInferenceResult&) {
        std::lock_guard<std::mutex> lock(mutex);
        ++completion_calls;
    });
    scheduler.start();
    assert(scheduler.submit("one", frame(1)));
    assert(wait_for([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return executor_calls == 1;
    }));
    assert(scheduler.submit("one", frame(2)));
    scheduler.stop();

    std::lock_guard<std::mutex> lock(mutex);
    assert(executor_calls == 1);
    assert(completion_calls == 0);
}

void verify_unregister_waits_for_callback() {
    NpuInferenceScheduler scheduler({false, "global_serial", "0_1_2", 1000, 1000, 10});
    std::mutex mutex;
    bool callback_started = false;
    scheduler.set_executor([](const NpuChannelConfig&, const Frame&) { return NpuInferenceResult{}; });
    scheduler.register_channel({"one", 1000, 1}, [&](const NpuInferenceResult&) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            callback_started = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    });
    scheduler.start();
    assert(scheduler.submit("one", frame(1)));
    assert(wait_for([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return callback_started;
    }));
    const auto started_at = Clock::now();
    scheduler.unregister_channel("one");
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at).count();
    scheduler.stop();
    assert(elapsed >= 20);
}

}  // namespace

int main() {
    verify_latest_frame_mailbox();
    verify_round_robin();
    verify_rate_limit_and_stale_drop();
    verify_stop_discards_pending_without_callback();
    verify_unregister_waits_for_callback();
    std::cout << "npu_scheduler_smoke: OK\n";
    return 0;
}
