#include "infer/NpuInferenceScheduler.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <utility>

struct NpuInferenceScheduler::ChannelState {
    NpuChannelConfig config;
    Completion completion;
    std::optional<Frame> latest_frame;
    std::chrono::steady_clock::time_point latest_submitted_at{};
    std::chrono::steady_clock::time_point next_eligible_at{};
    bool running = false;
    bool registered = true;
    size_t callbacks_in_flight = 0;
    NpuChannelStats stats;
    std::vector<float> npu_samples;
    std::vector<float> queue_wait_samples;
    std::vector<float> e2e_samples;
};

struct NpuInferenceScheduler::PendingJob {
    NpuChannelConfig channel;
    Frame frame;
    Completion completion;
    std::chrono::steady_clock::time_point submitted_at{};
    std::chrono::steady_clock::time_point dispatched_at{};
};

namespace {

using Clock = std::chrono::steady_clock;

void append_sample(std::vector<float>* samples, float value) {
    constexpr size_t kMaxSamples = 512;
    if (samples->size() == kMaxSamples) samples->erase(samples->begin());
    samples->push_back(value);
}

float average(const std::vector<float>& values) {
    if (values.empty()) return 0.0f;
    float total = 0.0f;
    for (const float value : values) total += value;
    return total / values.size();
}

float percentile95(std::vector<float> values) {
    if (values.empty()) return 0.0f;
    const size_t index = static_cast<size_t>(std::ceil(values.size() * 0.95f)) - 1;
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[index];
}

}  // namespace

NpuInferenceScheduler::NpuInferenceScheduler(NpuSchedulerConfig config)
    : config_(std::move(config)) {}

NpuInferenceScheduler::~NpuInferenceScheduler() {
    stop();
}

void NpuInferenceScheduler::set_executor(Executor executor) {
    if (!executor) {
        throw std::invalid_argument("NpuInferenceScheduler executor must not be empty");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_running_) {
        throw std::logic_error("NpuInferenceScheduler executor cannot change while running");
    }
    executor_ = std::move(executor);
}

void NpuInferenceScheduler::register_channel(NpuChannelConfig channel, Completion completion) {
    if (channel.channel_id.empty() || channel.target_infer_fps <= 0 || channel.weight < 1) {
        throw std::invalid_argument("NpuInferenceScheduler channel requires id, target_infer_fps > 0 and weight >= 1");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (channels_.count(channel.channel_id) != 0) {
        throw std::invalid_argument("NpuInferenceScheduler duplicate channel: " + channel.channel_id);
    }
    const std::string channel_id = channel.channel_id;
    ChannelState state;
    state.stats.channel_id = channel_id;
    state.config = std::move(channel);
    state.completion = std::move(completion);
    channels_.emplace(channel_id, std::move(state));
    channel_order_.push_back(channel_id);
    wakeup_.notify_all();
}

void NpuInferenceScheduler::unregister_channel(const std::string& channel_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto it = channels_.find(channel_id);
    if (it == channels_.end()) return;
    it->second.registered = false;
    it->second.latest_frame.reset();
    it->second.completion = {};
    wakeup_.notify_all();
    wakeup_.wait(lock, [&] {
        const auto current = channels_.find(channel_id);
        return current == channels_.end() ||
               (!current->second.running && current->second.callbacks_in_flight == 0);
    });
    channels_.erase(channel_id);
    channel_order_.erase(std::remove(channel_order_.begin(), channel_order_.end(), channel_id), channel_order_.end());
    if (channel_order_.empty()) {
        next_round_robin_index_ = 0;
    } else {
        next_round_robin_index_ %= channel_order_.size();
    }
    wakeup_.notify_all();
}

bool NpuInferenceScheduler::submit(const std::string& channel_id, Frame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) return false;
    const auto it = channels_.find(channel_id);
    if (it == channels_.end() || !it->second.registered) return false;

    ChannelState& state = it->second;
    ++state.stats.received;
    if (state.latest_frame) {
        ++state.stats.mailbox_replaced;
        ++stats_.mailbox_replaced;
    }
    if (state.running || Clock::now() < state.next_eligible_at) {
        ++state.stats.throttled;
    }
    state.latest_frame = std::move(frame);
    state.latest_submitted_at = Clock::now();
    wakeup_.notify_one();
    return true;
}

void NpuInferenceScheduler::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (worker_running_) return;
    if (!executor_) {
        throw std::logic_error("NpuInferenceScheduler requires an executor before start");
    }
    accepting_ = true;
    stop_requested_ = false;
    worker_running_ = true;
    stats_window_started_at_ = Clock::now();
    worker_ = std::thread(&NpuInferenceScheduler::run, this);
}

void NpuInferenceScheduler::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_running_) return;
        accepting_ = false;
        stop_requested_ = true;
        for (auto& [_, state] : channels_) state.latest_frame.reset();
    }
    wakeup_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool NpuInferenceScheduler::is_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return worker_running_;
}

NpuSchedulerStats NpuInferenceScheduler::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    NpuSchedulerStats snapshot = stats_;
    snapshot.channels.reserve(channels_.size());
    for (const auto& id : channel_order_) {
        const auto it = channels_.find(id);
        if (it != channels_.end()) {
            NpuChannelStats channel = it->second.stats;
            const float elapsed_sec = std::max(0.001f, std::chrono::duration<float>(
                Clock::now() - stats_window_started_at_).count());
            channel.actual_fps = channel.completed / elapsed_sec;
            channel.npu_avg_ms = average(it->second.npu_samples);
            channel.npu_p95_ms = percentile95(it->second.npu_samples);
            channel.queue_wait_avg_ms = average(it->second.queue_wait_samples);
            channel.queue_wait_p95_ms = percentile95(it->second.queue_wait_samples);
            channel.e2e_p95_ms = percentile95(it->second.e2e_samples);
            snapshot.channels.push_back(std::move(channel));
        }
    }
    const float elapsed_sec = std::max(0.001f, std::chrono::duration<float>(
        Clock::now() - stats_window_started_at_).count());
    snapshot.npu_avg_ms = average(npu_samples_);
    snapshot.npu_p95_ms = percentile95(npu_samples_);
    snapshot.queue_wait_avg_ms = average(queue_wait_samples_);
    snapshot.queue_wait_p95_ms = percentile95(queue_wait_samples_);
    snapshot.e2e_p95_ms = percentile95(e2e_samples_);
    snapshot.busy_ratio = busy_ms_ / (elapsed_sec * 1000.0f);
    return snapshot;
}

void NpuInferenceScheduler::drop_stale_locked(Clock::time_point now) {
    if (config_.max_frame_age_ms == 0) return;
    const int64_t cutoff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() - config_.max_frame_age_ms;
    for (auto& [_, state] : channels_) {
        if (state.latest_frame && state.latest_frame->timestamp_ms > 0 &&
            state.latest_frame->timestamp_ms < cutoff_ms) {
            state.latest_frame.reset();
            ++state.stats.stale_dropped;
            ++stats_.stale_dropped;
        }
    }
}

bool NpuInferenceScheduler::select_job_locked(PendingJob* job, Clock::time_point* wake_at) {
    const Clock::time_point now = Clock::now();
    drop_stale_locked(now);
    *wake_at = Clock::time_point::max();
    if (channel_order_.empty()) return false;

    const size_t count = channel_order_.size();
    next_round_robin_index_ %= count;
    for (size_t offset = 0; offset < count; ++offset) {
        const size_t index = (next_round_robin_index_ + offset) % count;
        const auto it = channels_.find(channel_order_[index]);
        if (it == channels_.end()) continue;
        ChannelState& state = it->second;
        if (!state.registered || !state.latest_frame || state.running) continue;

        if (now < state.next_eligible_at) {
            *wake_at = std::min(*wake_at, state.next_eligible_at);
            continue;
        }

        job->channel = state.config;
        job->frame = std::move(*state.latest_frame);
        job->completion = state.completion;
        job->submitted_at = state.latest_submitted_at;
        job->dispatched_at = now;
        state.latest_frame.reset();
        state.running = true;
        state.next_eligible_at = now + std::chrono::milliseconds(1000 / state.config.target_infer_fps);
        ++state.stats.dispatched;
        ++stats_.dispatched;
        next_round_robin_index_ = (index + 1) % count;
        return true;
    }
    return false;
}

void NpuInferenceScheduler::emit_stats_if_due_locked(Clock::time_point now) {
    if (stats_window_started_at_ == Clock::time_point{} ||
        now - stats_window_started_at_ < std::chrono::seconds(config_.stats_interval_sec)) {
        return;
    }
    NpuSchedulerStats snapshot = stats_;
    snapshot.npu_avg_ms = average(npu_samples_);
    snapshot.npu_p95_ms = percentile95(npu_samples_);
    snapshot.queue_wait_avg_ms = average(queue_wait_samples_);
    snapshot.queue_wait_p95_ms = percentile95(queue_wait_samples_);
    snapshot.e2e_p95_ms = percentile95(e2e_samples_);
    const float elapsed_sec = std::chrono::duration<float>(now - stats_window_started_at_).count();
    snapshot.busy_ratio = busy_ms_ / std::max(1.0f, elapsed_sec * 1000.0f);
    snapshot.channels.reserve(channels_.size());
    for (const auto& id : channel_order_) {
        const auto it = channels_.find(id);
        if (it == channels_.end()) continue;
        NpuChannelStats channel = it->second.stats;
        channel.actual_fps = channel.completed / std::max(0.001f, elapsed_sec);
        channel.npu_avg_ms = average(it->second.npu_samples);
        channel.npu_p95_ms = percentile95(it->second.npu_samples);
        channel.queue_wait_avg_ms = average(it->second.queue_wait_samples);
        channel.queue_wait_p95_ms = percentile95(it->second.queue_wait_samples);
        channel.e2e_p95_ms = percentile95(it->second.e2e_samples);
        snapshot.channels.push_back(std::move(channel));
    }
    std::cout << std::fixed << std::setprecision(2)
              << "[NpuScheduler] global dispatched=" << snapshot.dispatched
              << " completed=" << snapshot.completed
              << " npu_avg_ms=" << snapshot.npu_avg_ms
              << " npu_p95_ms=" << snapshot.npu_p95_ms
              << " busy_ratio=" << snapshot.busy_ratio
              << " queue_wait_p95_ms=" << snapshot.queue_wait_p95_ms
              << " stale_dropped=" << snapshot.stale_dropped << "\n";
    for (const auto& channel : snapshot.channels) {
        std::cout << "[NpuScheduler] channel=" << channel.channel_id
                  << " target=" << channels_.at(channel.channel_id).config.target_infer_fps
                  << " actual=" << channel.actual_fps
                  << " mailbox_replaced=" << channel.mailbox_replaced
                  << " throttled=" << channel.throttled
                  << " stale_dropped=" << channel.stale_dropped
                  << " wait_p95_ms=" << channel.queue_wait_p95_ms
                  << " e2e_p95_ms=" << channel.e2e_p95_ms
                  << " failures=" << channel.failed << "\n";
    }
    stats_ = {};
    stats_.channels.clear();
    for (auto& [_, state] : channels_) {
        state.stats.received = 0;
        state.stats.mailbox_replaced = 0;
        state.stats.stale_dropped = 0;
        state.stats.throttled = 0;
        state.stats.dispatched = 0;
        state.stats.completed = 0;
        state.stats.failed = 0;
        state.npu_samples.clear();
        state.queue_wait_samples.clear();
        state.e2e_samples.clear();
    }
    npu_samples_.clear();
    queue_wait_samples_.clear();
    e2e_samples_.clear();
    busy_ms_ = 0.0f;
    stats_window_started_at_ = now;
}

void NpuInferenceScheduler::run() {
    for (;;) {
        PendingJob job;
        Executor executor;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                if (stop_requested_) {
                    worker_running_ = false;
                    return;
                }
                emit_stats_if_due_locked(Clock::now());
                Clock::time_point wake_at;
                if (select_job_locked(&job, &wake_at)) {
                    executor = executor_;
                    break;
                }
                const Clock::time_point stats_due = stats_window_started_at_ +
                    std::chrono::seconds(config_.stats_interval_sec);
                wake_at = std::min(wake_at, stats_due);
                if (wake_at == Clock::time_point::max()) {
                    wakeup_.wait(lock);
                } else {
                    wakeup_.wait_until(lock, wake_at);
                }
            }
        }

        const Clock::time_point executor_started_at = Clock::now();
        NpuInferenceResult result = executor(job.channel, job.frame);
        const Clock::time_point executor_finished_at = Clock::now();
        result.channel_id = job.channel.channel_id;
        result.frame_id = job.frame.frame_id;
        result.frame_width = job.frame.width;
        result.frame_height = job.frame.height;
        result.pts_ms = job.frame.pts_ms;
        result.timestamp_ms = job.frame.timestamp_ms;

        Completion completion;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = channels_.find(job.channel.channel_id);
            if (it != channels_.end()) {
                ChannelState& state = it->second;
                state.running = false;
                if (result.success) {
                    ++state.stats.completed;
                    ++stats_.completed;
                } else {
                    ++state.stats.failed;
                    ++stats_.failed;
                }
                const float queue_wait_ms = std::chrono::duration<float, std::milli>(
                    job.dispatched_at - job.submitted_at).count();
                const float e2e_ms = std::chrono::duration<float, std::milli>(
                    executor_finished_at - job.submitted_at).count();
                append_sample(&state.npu_samples, result.npu_ms);
                append_sample(&state.queue_wait_samples, queue_wait_ms);
                append_sample(&state.e2e_samples, e2e_ms);
                append_sample(&npu_samples_, result.npu_ms);
                append_sample(&queue_wait_samples_, queue_wait_ms);
                append_sample(&e2e_samples_, e2e_ms);
                busy_ms_ += std::chrono::duration<float, std::milli>(
                    executor_finished_at - executor_started_at).count();
                if (!stop_requested_ && state.registered && job.completion) {
                    ++state.callbacks_in_flight;
                    completion = job.completion;
                }
            }
            wakeup_.notify_all();
        }
        if (completion) {
            completion(result);
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = channels_.find(job.channel.channel_id);
            if (it != channels_.end() && it->second.callbacks_in_flight > 0) {
                --it->second.callbacks_in_flight;
            }
            wakeup_.notify_all();
        }
    }
}
