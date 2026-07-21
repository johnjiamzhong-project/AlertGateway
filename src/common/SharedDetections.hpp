#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>
#include "common/Frame.hpp"

// display_mode: 0=原始最新检测框，1=自适应低延迟滤波（默认）。
struct TrackerConfig {
    int display_mode = 1;
    int confirm_hits = 2;
    int ttl_ms = 300;
    float match_iou = 0.20f;
    float center_distance_ratio = 0.12f;
    float ema_alpha = 0.25f;
    float deadzone_center_px = 6.0f;
    float deadzone_size_ratio = 0.01f;
    float innovation_iou = 0.45f;
    float max_correction_px = 120.0f;
    int jump_confirm_hits = 2;
    bool align_to_video_pts = true;
    bool adaptive_filter = true;
    float center_alpha_min = 0.18f;
    float center_alpha_max = 0.90f;
    float size_alpha_min = 0.12f;
    float size_alpha_max = 0.45f;
    bool center_gated_size_filter = false;
    float low_motion_size_alpha_max = 0.20f;
    float motion_full_response_ratio = 1.20f;
    float motion_smoothing_alpha = 0.35f;
    int display_hold_ms = 100;
    bool debug_logging = false;
    bool reversal_damping_enabled = false;
    float reversal_center_alpha_max = 0.35f;
    float reversal_min_motion_ratio = 0.005f;
    // 仅用于诊断，不改变轨迹或叠框逻辑；按 Active Track 的稳健中心统计整体运动。
    bool motion_stats_logging = false;
    // 使用同一检测帧至少三个 Active Track 的中位共同位移平滑慢速镜头运动；不外推未来位置。
    bool global_motion_center_filter = false;
    float global_motion_smoothing_alpha = 0.25f;
    int global_motion_min_tracks = 3;
};

struct DisplayDetections {
    std::vector<Detection> detections;
    uint64_t detection_frame_id = 0;
    int64_t detection_pts_ms = 0;
    int64_t pts_delta_ms = 0;
    size_t active_tracks = 0;
};

struct SharedDetections {
    explicit SharedDetections(TrackerConfig config = {}) : config_(config) {}

    void update(const std::vector<Detection>& dets, uint64_t frame_id, int64_t pts_ms,
                int width, int height) {
        std::lock_guard<std::mutex> lk(mutex_);
        last_raw_ = dets;
        last_detection_frame_id_ = frame_id;
        last_detection_pts_ms_ = pts_ms;
        last_matched_ = last_created_ = last_lost_ = 0;
        if (config_.display_mode == 0) {
            publish_snapshot(frame_id, pts_ms, last_raw_);
            return;
        }

        std::vector<Candidate> candidates;
        for (size_t ti = 0; ti < tracks_.size(); ++ti) {
            const Detection& association_box = tracks_[ti].smooth;
            for (size_t di = 0; di < dets.size(); ++di) {
                const bool same_class = tracks_[ti].smooth.class_id == dets[di].class_id;
                const float overlap = iou(association_box, dets[di]);
                const float distance = center_distance(association_box, dets[di]);
                const float max_distance = std::max(48.0f,
                    std::hypot(static_cast<float>(width), static_cast<float>(height)) * config_.center_distance_ratio);
                const float track_w = std::max(1.0f, association_box.x2 - association_box.x1);
                const float track_h = std::max(1.0f, association_box.y2 - association_box.y1);
                const float det_w = std::max(1.0f, dets[di].x2 - dets[di].x1);
                const float det_h = std::max(1.0f, dets[di].y2 - dets[di].y1);
                const float size_similarity = std::min(track_w * track_h, det_w * det_h) /
                                              std::max(track_w * track_h, det_w * det_h);
                const float local_distance = std::min(max_distance, 1.25f *
                    std::max(std::hypot(track_w, track_h), std::hypot(det_w, det_h)));
                const bool distance_match = distance <= std::max(48.0f, local_distance) &&
                                            size_similarity >= 0.40f;
                // 同类别检测在前级通常已经做过包含去重，但检测框会随帧抖动、
                // ROI 复检切换而改变尺寸。若旧框与新框仍是高 IoS 的包含关系，
                // 不能因为 IoU/尺寸相似度偏低而另建一条轨迹，否则 display_hold
                // 期间会把旧框和新框同时发布出来。
                const float intersection = intersection_area(association_box, dets[di]);
                const float smaller_area = std::min(box_area(association_box), box_area(dets[di]));
                const float containment = smaller_area > 0.0f ? intersection / smaller_area : 0.0f;
                const Detection& inner = box_area(association_box) <= box_area(dets[di])
                    ? association_box : dets[di];
                const Detection& outer = box_area(association_box) <= box_area(dets[di])
                    ? dets[di] : association_box;
                const bool containment_match = containment >= kTrackContainmentThreshold &&
                                               center_inside(inner, outer);
                // 跨类别的近同框或强包含框不是两条可同时展示的轨迹；它们是同一
                // 位置的竞争候选。先关联到原轨迹，后续由两帧确认再决定是否切换。
                const bool cross_class_competitor = !same_class &&
                    (overlap >= kCrossClassTrackDuplicateIoU ||
                     (containment >= kCrossClassContainmentThreshold && center_inside(inner, outer)));
                if (same_class) {
                    if (overlap < config_.match_iou && !distance_match && !containment_match) continue;
                } else if (!cross_class_competitor) {
                    continue;
                }
                const float active_bonus = tracks_[ti].state == TrackState::Active ? 0.05f : 0.0f;
                candidates.push_back({ti, di, overlap, distance, cross_class_competitor,
                    overlap + size_similarity * 0.10f + (same_class ? containment * 0.10f : 0.05f) -
                    distance / max_distance * 0.05f + active_bonus});
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        std::vector<Candidate> selected_candidates;
        std::vector<bool> selected_tracks(tracks_.size(), false), selected_dets(dets.size(), false);
        for (const Candidate& candidate : candidates) {
            if (selected_tracks[candidate.track] || selected_dets[candidate.detection]) continue;
            selected_tracks[candidate.track] = true;
            selected_dets[candidate.detection] = true;
            selected_candidates.push_back(candidate);
        }

        bool global_motion_used = false;
        float global_motion_dx = 0.0f;
        float global_motion_dy = 0.0f;
        if (config_.global_motion_center_filter) {
            struct GlobalMotionSample { float dx, dy; };
            std::vector<GlobalMotionSample> global_samples;
            for (const Candidate& candidate : selected_candidates) {
                const Track& track = tracks_[candidate.track];
                const int64_t dt_ms = pts_ms - track.last_detection_pts_ms;
                if (track.state != TrackState::Active || dt_ms < 10 || dt_ms > 250) continue;
                const Detection& measured = dets[candidate.detection];
                global_samples.push_back({
                    (measured.x1 + measured.x2 - track.last_measurement.x1 - track.last_measurement.x2) * 0.5f,
                    (measured.y1 + measured.y2 - track.last_measurement.y1 - track.last_measurement.y2) * 0.5f
                });
            }
            if (global_samples.size() >= static_cast<size_t>(std::max(1, config_.global_motion_min_tracks))) {
                const auto median = [](std::vector<float>& values) {
                    const size_t middle = values.size() / 2;
                    std::nth_element(values.begin(), values.begin() + middle, values.end());
                    float result = values[middle];
                    if ((values.size() & 1u) == 0u) {
                        result = (result + *std::max_element(values.begin(), values.begin() + middle)) * 0.5f;
                    }
                    return result;
                };
                std::vector<float> global_dxs, global_dys;
                global_dxs.reserve(global_samples.size());
                global_dys.reserve(global_samples.size());
                for (const GlobalMotionSample& sample : global_samples) {
                    global_dxs.push_back(sample.dx);
                    global_dys.push_back(sample.dy);
                }
                float measured_dx = median(global_dxs);
                float measured_dy = median(global_dys);
                const bool stale = !global_motion_valid_ ||
                    pts_ms - global_motion_pts_ms_ > 250 || pts_ms <= global_motion_pts_ms_;
                const float alpha = std::clamp(config_.global_motion_smoothing_alpha, 0.0f, 1.0f);
                if (stale) {
                    filtered_global_motion_dx_ = measured_dx;
                    filtered_global_motion_dy_ = measured_dy;
                } else {
                    filtered_global_motion_dx_ = alpha * measured_dx +
                        (1.0f - alpha) * filtered_global_motion_dx_;
                    filtered_global_motion_dy_ = alpha * measured_dy +
                        (1.0f - alpha) * filtered_global_motion_dy_;
                }
                global_motion_pts_ms_ = pts_ms;
                global_motion_valid_ = true;
                global_motion_dx = filtered_global_motion_dx_;
                global_motion_dy = filtered_global_motion_dy_;
                global_motion_used = true;
            }
        }

        std::vector<bool> matched_tracks(tracks_.size(), false), matched_dets(dets.size(), false);
        std::vector<MotionSample> matched_motion;
        for (const Candidate& c : selected_candidates) {
            Track& track = tracks_[c.track];
            track.last_reversal_damped = false;
            const bool was_active = track.state == TrackState::Active;
            const Detection prior_smooth = track.smooth;
            bool cross_class_switch_confirmed = false;
            if (c.cross_class_competitor) {
                if (track.cross_class_pending_hits > 0 &&
                    track.cross_class_pending.class_id == dets[c.detection].class_id &&
                    is_cross_class_competitor(track.cross_class_pending, dets[c.detection])) {
                    ++track.cross_class_pending_hits;
                } else {
                    track.cross_class_pending = dets[c.detection];
                    track.cross_class_pending_hits = 1;
                }
                matched_tracks[c.track] = matched_dets[c.detection] = true;
                // 单帧误分类时保持旧稳定框，同时吃掉竞争检测，绝不再创建第二条轨迹。
                if (track.cross_class_pending_hits < kCrossClassSwitchConfirmHits) {
                    track.last_detection_pts_ms = pts_ms;
                    track.misses = 0;
                    if (config_.debug_logging) {
                        std::cout << "[TrackExclusive] pending track=" << track.id
                                  << " old=" << prior_smooth.label
                                  << " challenger=" << dets[c.detection].label << "\n";
                    }
                    continue;
                }
                cross_class_switch_confirmed = true;
                track.cross_class_pending_hits = 0;
                if (config_.debug_logging) {
                    std::cout << "[TrackExclusive] confirmed track=" << track.id
                              << " old=" << prior_smooth.label
                              << " new=" << dets[c.detection].label << "\n";
                }
            } else {
                track.cross_class_pending_hits = 0;
            }
            const float dt_sec = std::max(0.001f, static_cast<float>(pts_ms - track.last_detection_pts_ms) / 1000.0f);
            const float raw_dx = (dets[c.detection].x1 + dets[c.detection].x2 -
                                 track.last_measurement.x1 - track.last_measurement.x2) * 0.5f;
            const float raw_dy = (dets[c.detection].y1 + dets[c.detection].y2 -
                                 track.last_measurement.y1 - track.last_measurement.y2) * 0.5f;
            const bool innovation = c.distance > config_.max_correction_px &&
                                    c.overlap < config_.innovation_iou;
            const bool requires_innovation_confirmation =
                innovation && track.state != TrackState::Active;
            // 已确认且正在显示的轨迹直接做有限校正，避免快速运动时每隔一帧冻结一次。
            // Tentative/Shadow 仍需二次确认，防止单次错误检测建立或恢复错误轨迹。
            if (requires_innovation_confirmation) {
                if (track.pending_hits > 0 &&
                    center_distance(track.pending, dets[c.detection]) <= config_.max_correction_px) {
                    ++track.pending_hits;
                } else {
                    track.pending = dets[c.detection];
                    track.pending_hits = 1;
                }
                matched_tracks[c.track] = matched_dets[c.detection] = true;
                if (track.pending_hits < config_.jump_confirm_hits) {
                    log_update(track, frame_id, pts_ms, dt_sec, dets[c.detection], prior_smooth,
                               c.overlap, c.distance, false, false, true, false);
                    continue;
                }
            }
            const bool deadzone = in_deadzone(prior_smooth, dets[c.detection]);
            Detection smooth_measurement = prior_smooth;
            bool filter_reset = false;
            float center_alpha = config_.ema_alpha;
            float size_alpha = config_.ema_alpha;
            float effective_size_alpha_max = config_.size_alpha_max;
            if (!deadzone) {
                // 中心和尺寸分开滤波。运动量按目标自身尺寸归一化，因此不依赖 4K 固定像素阈值。
                // 静止时中心/尺寸强平滑；快速运动时只让中心快速跟随，尺寸仍保持较强抑噪。
                filter_reset = track.state != TrackState::Active || c.overlap < config_.match_iou ||
                               cross_class_switch_confirmed;
                if (filter_reset) {
                    smooth_measurement = dets[c.detection];
                    track.filtered_center_motion = 0.0f;
                    track.filtered_size_motion = 0.0f;
                    track.last_center_delta_valid = false;
                    center_alpha = size_alpha = 1.0f;
                } else if (config_.adaptive_filter) {
                    smooth_measurement = adaptive_box(track, prior_smooth, dets[c.detection], dt_sec,
                                                      &center_alpha, &size_alpha,
                                                      &effective_size_alpha_max,
                                                      global_motion_used ? global_motion_dx : 0.0f,
                                                      global_motion_used ? global_motion_dy : 0.0f);
                } else {
                    smooth_measurement = ema_box(prior_smooth, dets[c.detection], config_.ema_alpha);
                }
            } else {
                track.filtered_center_motion *= 1.0f - config_.motion_smoothing_alpha;
                track.filtered_size_motion *= 1.0f - config_.motion_smoothing_alpha;
                center_alpha = size_alpha = 0.0f;
            }
            track.last_measurement = dets[c.detection];
            track.last_center_alpha = center_alpha;
            track.last_size_alpha = size_alpha;
            track.last_effective_size_alpha_max = effective_size_alpha_max;
            track.last_global_motion_used = global_motion_used && !deadzone && !filter_reset;
            track.last_global_motion_dx = track.last_global_motion_used ? global_motion_dx : 0.0f;
            track.last_global_motion_dy = track.last_global_motion_used ? global_motion_dy : 0.0f;
            smooth_measurement.score = dets[c.detection].score;
            // 单次校正有上限，避免一个大偏差把显示框直接拉离目标。
            if (!filter_reset) {
                smooth_measurement = clamp_center_correction(prior_smooth, smooth_measurement,
                                                              config_.max_correction_px);
            }
            const bool innovation_confirmed = requires_innovation_confirmation &&
                                              track.pending_hits >= config_.jump_confirm_hits;
            track.pending_hits = 0;
            track.smooth = smooth_measurement;
            track.last_detection_pts_ms = pts_ms;
            ++track.hits; track.misses = 0;
            if (track.hits >= config_.confirm_hits) track.state = TrackState::Active;
            matched_tracks[c.track] = matched_dets[c.detection] = true;
            ++last_matched_;
            log_motion_track(track, frame_id, pts_ms, dt_sec, dets[c.detection],
                             deadzone, filter_reset, was_active);
            if (was_active) {
                matched_motion.push_back({track.id, dets[c.detection], track.smooth, raw_dx, raw_dy});
            }
            log_update(track, frame_id, pts_ms, dt_sec, dets[c.detection], smooth_measurement,
                       c.overlap, c.distance, deadzone, false, innovation, innovation_confirmed);
        }
        for (size_t di = 0; di < dets.size(); ++di) {
            if (matched_dets[di]) continue;
            Track track{next_track_id_++, dets[di], pts_ms, 1, 0};
            track.last_measurement = dets[di];
            track.state = config_.confirm_hits <= 1 ? TrackState::Active : TrackState::Tentative;
            tracks_.push_back(track);
            ++last_created_;
            log_motion_track(track, frame_id, pts_ms, 0.0f, dets[di],
                             false, true, false);
            log_update(track, frame_id, pts_ms, 0.0f, dets[di], dets[di],
                       0.0f, 0.0f, false, true, false, false);
        }
        for (size_t ti = 0; ti < matched_tracks.size(); ++ti)
            if (!matched_tracks[ti]) ++tracks_[ti].misses;
        for (Track& track : tracks_) {
            if (track.state == TrackState::Active &&
                pts_ms - track.last_detection_pts_ms > config_.display_hold_ms) {
                track.state = TrackState::Shadow;
            }
        }
        const auto before = tracks_.size();
        tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(), [&](const Track& t) {
            return pts_ms - t.last_detection_pts_ms > config_.ttl_ms ||
                   (t.hits < config_.confirm_hits && t.misses > 0);
        }), tracks_.end());
        last_lost_ = before - tracks_.size();
        last_lost_ += deduplicate_tracks(pts_ms);
        std::vector<Detection> snapshot_detections;
        for (const Track& track : tracks_)
            if (is_displayable(track, pts_ms)) snapshot_detections.push_back(track.smooth);
        deduplicate_display_detections(snapshot_detections);
        update_motion_stats(matched_motion, frame_id, pts_ms);
        publish_snapshot(frame_id, pts_ms, std::move(snapshot_detections));
    }

    DisplayDetections get_for_pts(int64_t current_pts_ms) const {
        std::lock_guard<std::mutex> lk(mutex_);
        DisplayDetections result;
        result.detection_frame_id = last_detection_frame_id_;
        result.detection_pts_ms = last_detection_pts_ms_;
        result.pts_delta_ms = current_pts_ms - last_detection_pts_ms_;
        result.active_tracks = config_.display_mode == 0 ? last_raw_.size() : count_displayable(current_pts_ms);
        if (config_.align_to_video_pts) {
            for (auto it = snapshots_.rbegin(); it != snapshots_.rend(); ++it) {
                if (it->pts_ms > current_pts_ms) continue;
                result.detections = it->detections;
                if (config_.display_mode != 0) deduplicate_display_detections(result.detections);
                result.detection_frame_id = it->frame_id;
                result.detection_pts_ms = it->pts_ms;
                result.pts_delta_ms = current_pts_ms - it->pts_ms;
                result.active_tracks = result.detections.size();
                return result;
            }
            return result;
        }
        if (config_.display_mode == 0) { result.detections = last_raw_; return result; }
        for (const Track& track : tracks_) {
            if (!is_displayable(track, current_pts_ms)) continue;
            result.detections.push_back(track.smooth);
        }
        return result;
    }

    std::vector<Detection> get() const {
        std::lock_guard<std::mutex> lk(mutex_);
        if (config_.display_mode == 0) return last_raw_;
        std::vector<Detection> result;
        for (const Track& track : tracks_)
            if (is_displayable(track, last_detection_pts_ms_)) result.push_back(track.smooth);
        deduplicate_display_detections(result);
        return result;
    }
    void get_debug_stats(size_t& matched, size_t& created, size_t& lost) const {
        std::lock_guard<std::mutex> lk(mutex_); matched=last_matched_; created=last_created_; lost=last_lost_;
    }
    uint64_t get_reversal_damped_total() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return total_reversal_damped_;
    }

private:
    enum class TrackState { Tentative, Active, Shadow };
    static constexpr float kCrossClassTrackDuplicateIoU = 0.85f;
    static constexpr float kTrackContainmentThreshold = 0.70f;
    static constexpr float kCrossClassContainmentThreshold = 0.50f;
    static constexpr int kCrossClassSwitchConfirmHits = 2;
    struct Track {
        uint64_t id;
        Detection smooth;
        int64_t last_detection_pts_ms;
        int hits, misses;
        Detection pending{};
        int pending_hits = 0;
        Detection cross_class_pending{};
        int cross_class_pending_hits = 0;
        TrackState state = TrackState::Tentative;
        Detection last_measurement{};
        float filtered_center_motion = 0.0f;
        float filtered_size_motion = 0.0f;
        float last_center_alpha = 1.0f;
        float last_size_alpha = 1.0f;
        float last_effective_size_alpha_max = 0.45f;
        float last_center_dx = 0.0f;
        float last_center_dy = 0.0f;
        bool last_center_delta_valid = false;
        bool last_reversal_damped = false;
        bool last_global_motion_used = false;
        float last_global_motion_dx = 0.0f;
        float last_global_motion_dy = 0.0f;
    };
    struct Snapshot { uint64_t frame_id; int64_t pts_ms; std::vector<Detection> detections; };
    struct Candidate { size_t track, detection; float overlap, distance; bool cross_class_competitor; float score; };
    struct MotionSample {
        uint64_t track_id;
        Detection raw;
        Detection smooth;
        float raw_dx;
        float raw_dy;
    };
    static std::pair<float, float> median_center(const std::vector<Detection>& boxes) {
        if (boxes.empty()) return {0.0f, 0.0f};
        std::vector<float> xs, ys;
        xs.reserve(boxes.size());
        ys.reserve(boxes.size());
        for (const Detection& box : boxes) {
            xs.push_back((box.x1 + box.x2) * 0.5f);
            ys.push_back((box.y1 + box.y2) * 0.5f);
        }
        const auto middle = [](std::vector<float>& values) {
            const size_t mid = values.size() / 2;
            std::nth_element(values.begin(), values.begin() + mid, values.end());
            float result = values[mid];
            if ((values.size() & 1u) == 0u) {
                const auto lower = std::max_element(values.begin(), values.begin() + mid);
                result = (result + *lower) * 0.5f;
            }
            return result;
        };
        return {middle(xs), middle(ys)};
    }
    static std::pair<float, float> median_size(const std::vector<Detection>& boxes) {
        if (boxes.empty()) return {0.0f, 0.0f};
        std::vector<float> widths, heights;
        widths.reserve(boxes.size());
        heights.reserve(boxes.size());
        for (const Detection& box : boxes) {
            widths.push_back(std::max(1.0f, box.x2 - box.x1));
            heights.push_back(std::max(1.0f, box.y2 - box.y1));
        }
        const auto middle = [](std::vector<float>& values) {
            const size_t mid = values.size() / 2;
            std::nth_element(values.begin(), values.begin() + mid, values.end());
            float result = values[mid];
            if ((values.size() & 1u) == 0u) {
                const auto lower = std::max_element(values.begin(), values.begin() + mid);
                result = (result + *lower) * 0.5f;
            }
            return result;
        };
        return {middle(widths), middle(heights)};
    }
    void update_motion_stats(const std::vector<MotionSample>& current,
                             uint64_t frame_id, int64_t pts_ms) {
        if (!config_.motion_stats_logging) return;
        if (current.empty()) {
            previous_motion_samples_.clear();
            previous_motion_pts_ms_ = 0;
            return;
        }
        std::vector<Detection> raw_current, smooth_current, raw_previous, smooth_previous;
        std::vector<uint64_t> active_ids, stable_ids;
        active_ids.reserve(current.size());
        stable_ids.reserve(current.size());
        size_t raw_reversal_count = 0;
        for (const MotionSample& sample : current) {
            active_ids.push_back(sample.track_id);
            const auto previous = std::find_if(previous_motion_samples_.begin(), previous_motion_samples_.end(),
                [&](const MotionSample& item) { return item.track_id == sample.track_id; });
            if (previous == previous_motion_samples_.end()) continue;
            stable_ids.push_back(sample.track_id);
            raw_current.push_back(sample.raw);
            smooth_current.push_back(sample.smooth);
            raw_previous.push_back(previous->raw);
            smooth_previous.push_back(previous->smooth);
            const float current_step = std::hypot(sample.raw_dx, sample.raw_dy);
            const float previous_step = std::hypot(previous->raw_dx, previous->raw_dy);
            if (current_step > 0.0f && previous_step > 0.0f &&
                sample.raw_dx * previous->raw_dx + sample.raw_dy * previous->raw_dy < 0.0f) {
                ++raw_reversal_count;
            }
        }
        const int64_t previous_pts_ms = previous_motion_pts_ms_;
        previous_motion_samples_ = current;
        previous_motion_pts_ms_ = pts_ms;
        const size_t stable_tracks = raw_current.size();
        if (stable_tracks == 0) return;
        const auto raw_center = median_center(raw_current);
        const auto smooth_center = median_center(smooth_current);
        const auto previous_raw_center = median_center(raw_previous);
        const auto previous_smooth_center = median_center(smooth_previous);
        const auto raw_size = median_size(raw_current);
        const auto smooth_size = median_size(smooth_current);
        const auto previous_raw_size = median_size(raw_previous);
        const auto previous_smooth_size = median_size(smooth_previous);
        const float raw_dx = raw_center.first - previous_raw_center.first;
        const float raw_dy = raw_center.second - previous_raw_center.second;
        const float smooth_dx = smooth_center.first - previous_smooth_center.first;
        const float smooth_dy = smooth_center.second - previous_smooth_center.second;
        const int64_t dt_ms = previous_pts_ms == 0 ? 0 : pts_ms - previous_pts_ms;
        const float center_gap = std::hypot(raw_center.first - smooth_center.first,
                                            raw_center.second - smooth_center.second);
        const float delta_gap = std::hypot(raw_dx - smooth_dx, raw_dy - smooth_dy);
        const float reference_diagonal = std::max(1.0f, std::hypot(raw_size.first, raw_size.second));
        const float center_gap_ratio = center_gap / reference_diagonal;
        const float raw_step_ratio = std::hypot(raw_dx, raw_dy) / reference_diagonal;
        const float smooth_step_ratio = std::hypot(smooth_dx, smooth_dy) / reference_diagonal;
        const float raw_size_step = std::hypot(
            std::log(raw_size.first / std::max(1.0f, previous_raw_size.first)),
            std::log(raw_size.second / std::max(1.0f, previous_raw_size.second)));
        const float smooth_size_step = std::hypot(
            std::log(smooth_size.first / std::max(1.0f, previous_smooth_size.first)),
            std::log(smooth_size.second / std::max(1.0f, previous_smooth_size.second)));
        std::sort(active_ids.begin(), active_ids.end());
        std::sort(stable_ids.begin(), stable_ids.end());
        std::cout << "[MotionStats] frame=" << frame_id
                  << " pts_ms=" << pts_ms
                  << " matched_active=" << current.size()
                  << " stable_tracks=" << stable_tracks
                  << " raw_center=" << raw_center.first << "," << raw_center.second
                  << " smooth_center=" << smooth_center.first << "," << smooth_center.second
                  << " raw_size=" << raw_size.first << "," << raw_size.second
                  << " smooth_size=" << smooth_size.first << "," << smooth_size.second
                  << " raw_delta=" << raw_dx << "," << raw_dy
                  << " smooth_delta=" << smooth_dx << "," << smooth_dy
                  << " dt_ms=" << dt_ms
                  << " center_gap=" << center_gap
                  << " center_gap_ratio=" << center_gap_ratio
                  << " delta_gap=" << delta_gap
                  << " raw_step_ratio=" << raw_step_ratio
                  << " smooth_step_ratio=" << smooth_step_ratio
                  << " raw_size_step=" << raw_size_step
                  << " smooth_size_step=" << smooth_size_step
                  << " raw_reversals=" << raw_reversal_count
                  << " created=" << last_created_
                  << " lost=" << last_lost_
                  << " active_ids=";
        for (size_t i = 0; i < active_ids.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << active_ids[i];
        }
        std::cout << " stable_ids=";
        for (size_t i = 0; i < stable_ids.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << stable_ids[i];
        }
        std::cout << '\n';
        if (frame_id % 30 == 0) std::cout << std::flush;
    }
    static float box_area(const Detection& box) {
        return std::max(0.0f, box.x2 - box.x1) * std::max(0.0f, box.y2 - box.y1);
    }
    static float intersection_area(const Detection& a, const Detection& b) {
        const float x1 = std::max(a.x1, b.x1);
        const float y1 = std::max(a.y1, b.y1);
        const float x2 = std::min(a.x2, b.x2);
        const float y2 = std::min(a.y2, b.y2);
        return std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    }
    static bool center_inside(const Detection& inner, const Detection& outer) {
        const float cx = (inner.x1 + inner.x2) * 0.5f;
        const float cy = (inner.y1 + inner.y2) * 0.5f;
        return cx >= outer.x1 && cx <= outer.x2 && cy >= outer.y1 && cy <= outer.y2;
    }
    static bool is_cross_class_competitor(const Detection& first, const Detection& second) {
        const float smaller_area = std::min(box_area(first), box_area(second));
        const float containment = smaller_area > 0.0f
            ? intersection_area(first, second) / smaller_area : 0.0f;
        const Detection& inner = box_area(first) <= box_area(second) ? first : second;
        const Detection& outer = box_area(first) <= box_area(second) ? second : first;
        return iou(first, second) >= kCrossClassTrackDuplicateIoU ||
               (containment >= kCrossClassContainmentThreshold && center_inside(inner, outer));
    }
    static bool is_duplicate_box(const Detection& first, const Detection& second) {
        if (first.class_id == second.class_id) {
            const float smaller_area = std::min(box_area(first), box_area(second));
            const float containment = smaller_area > 0.0f
                ? intersection_area(first, second) / smaller_area : 0.0f;
            const Detection& inner = box_area(first) <= box_area(second) ? first : second;
            const Detection& outer = box_area(first) <= box_area(second) ? second : first;
            return containment >= kTrackContainmentThreshold && center_inside(inner, outer);
        }
        return is_cross_class_competitor(first, second);
    }
    static void deduplicate_display_detections(std::vector<Detection>& detections) {
        if (detections.size() < 2) return;
        std::vector<bool> dropped(detections.size(), false);
        for (size_t i = 0; i < detections.size(); ++i) {
            if (dropped[i]) continue;
            for (size_t j = i + 1; j < detections.size(); ++j) {
                if (dropped[j] || !is_duplicate_box(detections[i], detections[j])) continue;
                if (detections[j].score > detections[i].score) {
                    dropped[i] = true;
                    break;
                }
                dropped[j] = true;
            }
        }
        size_t index = 0;
        detections.erase(std::remove_if(detections.begin(), detections.end(),
                                        [&dropped, &index](const Detection&) {
                                            return dropped[index++];
                                        }),
                         detections.end());
    }
    size_t deduplicate_tracks(int64_t pts_ms) {
        if (tracks_.size() < 2) return 0;
        std::vector<bool> dropped(tracks_.size(), false);
        for (size_t i = 0; i < tracks_.size(); ++i) {
            if (dropped[i]) continue;
            for (size_t j = i + 1; j < tracks_.size(); ++j) {
                if (dropped[j]) continue;
                const Detection& first = tracks_[i].smooth;
                const Detection& second = tracks_[j].smooth;
                if (!is_duplicate_box(first, second)) continue;

                // 优先保留当前仍可显示、最近刚被检测更新、分数更高的轨迹。
                // 这是轨迹状态合并，不是对低 IoU 的合法嵌套目标做类别无关 NMS。
                const bool first_displayable = is_displayable(tracks_[i], pts_ms);
                const bool second_displayable = is_displayable(tracks_[j], pts_ms);
                bool keep_second = false;
                if (first_displayable != second_displayable) {
                    keep_second = second_displayable;
                } else if (tracks_[i].last_detection_pts_ms != tracks_[j].last_detection_pts_ms) {
                    keep_second = tracks_[j].last_detection_pts_ms > tracks_[i].last_detection_pts_ms;
                } else if (tracks_[i].smooth.score != tracks_[j].smooth.score) {
                    keep_second = tracks_[j].smooth.score > tracks_[i].smooth.score;
                } else {
                    keep_second = tracks_[j].hits > tracks_[i].hits;
                }
                if (keep_second) {
                    dropped[i] = true;
                    break;
                }
                dropped[j] = true;
            }
        }
        const size_t before = tracks_.size();
        tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                                     [index = size_t{0}, &dropped](const Track&) mutable {
                                         return dropped[index++];
                                     }),
                       tracks_.end());
        return before - tracks_.size();
    }
    static float iou(const Detection& a, const Detection& b) {
        float x1=std::max(a.x1,b.x1), y1=std::max(a.y1,b.y1), x2=std::min(a.x2,b.x2), y2=std::min(a.y2,b.y2);
        float in=std::max(0.0f,x2-x1)*std::max(0.0f,y2-y1), aa=std::max(0.0f,a.x2-a.x1)*std::max(0.0f,a.y2-a.y1), ab=std::max(0.0f,b.x2-b.x1)*std::max(0.0f,b.y2-b.y1);
        return in/std::max(1.0f,aa+ab-in);
    }
    static float center_distance(const Detection& a, const Detection& b) { return std::hypot((a.x1+a.x2-b.x1-b.x2)*0.5f,(a.y1+a.y2-b.y1-b.y2)*0.5f); }
    bool is_displayable(const Track& track, int64_t pts_ms) const {
        return track.state == TrackState::Active && track.hits >= config_.confirm_hits &&
               pts_ms - track.last_detection_pts_ms <= config_.display_hold_ms;
    }
    size_t count_displayable(int64_t pts_ms) const {
        return static_cast<size_t>(std::count_if(tracks_.begin(), tracks_.end(),
            [&](const Track& track) { return is_displayable(track, pts_ms); }));
    }
    static Detection ema_box(const Detection& previous, const Detection& measured, float alpha) {
        const float pcx=(previous.x1+previous.x2)*0.5f, pcy=(previous.y1+previous.y2)*0.5f, pw=previous.x2-previous.x1, ph=previous.y2-previous.y1;
        const float mcx=(measured.x1+measured.x2)*0.5f, mcy=(measured.y1+measured.y2)*0.5f, mw=measured.x2-measured.x1, mh=measured.y2-measured.y1;
        Detection out=measured; const float cx=alpha*mcx+(1-alpha)*pcx, cy=alpha*mcy+(1-alpha)*pcy, w=alpha*mw+(1-alpha)*pw, h=alpha*mh+(1-alpha)*ph;
        out.x1=cx-w*0.5f; out.x2=cx+w*0.5f; out.y1=cy-h*0.5f; out.y2=cy+h*0.5f; return out;
    }
    Detection adaptive_box(Track& track, const Detection& previous, const Detection& measured,
                           float dt_sec, float* center_alpha_out, float* size_alpha_out,
                           float* effective_size_alpha_max_out,
                           float global_motion_dx, float global_motion_dy) {
        const float pcx = (previous.x1 + previous.x2) * 0.5f;
        const float pcy = (previous.y1 + previous.y2) * 0.5f;
        const float pw = std::max(1.0f, previous.x2 - previous.x1);
        const float ph = std::max(1.0f, previous.y2 - previous.y1);
        const float mcx = (measured.x1 + measured.x2) * 0.5f;
        const float mcy = (measured.y1 + measured.y2) * 0.5f;
        const float mw = std::max(1.0f, measured.x2 - measured.x1);
        const float mh = std::max(1.0f, measured.y2 - measured.y1);
        const float lcx = (track.last_measurement.x1 + track.last_measurement.x2) * 0.5f;
        const float lcy = (track.last_measurement.y1 + track.last_measurement.y2) * 0.5f;
        const float lw = std::max(1.0f, track.last_measurement.x2 - track.last_measurement.x1);
        const float lh = std::max(1.0f, track.last_measurement.y2 - track.last_measurement.y1);

        const float reference_diagonal = std::max(32.0f, std::hypot((lw + mw) * 0.5f, (lh + mh) * 0.5f));
        const float raw_center_motion = std::hypot(mcx - lcx, mcy - lcy) /
                                        reference_diagonal / std::max(0.001f, dt_sec);
        const float raw_size_motion = std::hypot(std::log(mw / lw), std::log(mh / lh)) /
                                      std::max(0.001f, dt_sec);
        const float derivative_alpha = std::clamp(config_.motion_smoothing_alpha, 0.0f, 1.0f);
        track.filtered_center_motion = derivative_alpha * raw_center_motion +
            (1.0f - derivative_alpha) * track.filtered_center_motion;
        track.filtered_size_motion = derivative_alpha * raw_size_motion +
            (1.0f - derivative_alpha) * track.filtered_size_motion;

        const float full_response = std::max(0.01f, config_.motion_full_response_ratio);
        const auto response = [full_response](float raw, float filtered) {
            float t = std::clamp(std::max(raw, filtered) / full_response, 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };
        const float center_response = response(raw_center_motion, track.filtered_center_motion);
        const float size_response = response(raw_size_motion, track.filtered_size_motion);
        float center_alpha = std::clamp(config_.center_alpha_min +
            (config_.center_alpha_max - config_.center_alpha_min) * center_response, 0.0f, 1.0f);
        float effective_size_alpha_max = config_.size_alpha_max;
        if (config_.center_gated_size_filter) {
            const float low_motion_max = std::clamp(config_.low_motion_size_alpha_max,
                                                     config_.size_alpha_min,
                                                     config_.size_alpha_max);
            effective_size_alpha_max = low_motion_max +
                (config_.size_alpha_max - low_motion_max) * center_response;
        }
        const float size_alpha = std::clamp(config_.size_alpha_min +
            (effective_size_alpha_max - config_.size_alpha_min) * size_response, 0.0f, 1.0f);
        const float center_dx = mcx - lcx;
        const float center_dy = mcy - lcy;
        const float center_delta = std::hypot(center_dx, center_dy);
        const float min_delta = std::max(0.0f, config_.reversal_min_motion_ratio) * reference_diagonal;
        if (config_.reversal_damping_enabled && track.last_center_delta_valid &&
            center_delta >= min_delta &&
            std::hypot(track.last_center_dx, track.last_center_dy) >= min_delta &&
            center_dx * track.last_center_dx + center_dy * track.last_center_dy < 0.0f) {
            center_alpha = std::min(center_alpha,
                std::clamp(config_.reversal_center_alpha_max, 0.0f, 1.0f));
            track.last_reversal_damped = true;
            ++total_reversal_damped_;
        }
        if (center_delta >= min_delta) {
            track.last_center_dx = center_dx;
            track.last_center_dy = center_dy;
            track.last_center_delta_valid = true;
        }
        if (center_alpha_out) *center_alpha_out = center_alpha;
        if (size_alpha_out) *size_alpha_out = size_alpha;
        if (effective_size_alpha_max_out) *effective_size_alpha_max_out = effective_size_alpha_max;

        const float predicted_cx = pcx + global_motion_dx;
        const float predicted_cy = pcy + global_motion_dy;
        const float cx = center_alpha * mcx + (1.0f - center_alpha) * predicted_cx;
        const float cy = center_alpha * mcy + (1.0f - center_alpha) * predicted_cy;
        const float width = size_alpha * mw + (1.0f - size_alpha) * pw;
        const float height = size_alpha * mh + (1.0f - size_alpha) * ph;
        Detection out = measured;
        out.x1 = cx - width * 0.5f;
        out.x2 = cx + width * 0.5f;
        out.y1 = cy - height * 0.5f;
        out.y2 = cy + height * 0.5f;
        return out;
    }
    bool in_deadzone(const Detection& previous, const Detection& measured) const {
        const float dc=center_distance(previous, measured), pw=std::max(1.0f,previous.x2-previous.x1), ph=std::max(1.0f,previous.y2-previous.y1);
        return dc < config_.deadzone_center_px && std::abs((measured.x2-measured.x1)-pw)/pw < config_.deadzone_size_ratio && std::abs((measured.y2-measured.y1)-ph)/ph < config_.deadzone_size_ratio;
    }
    static Detection clamp_center_correction(const Detection& previous, Detection candidate, float max_delta) {
        const float pcx=(previous.x1+previous.x2)*0.5f, pcy=(previous.y1+previous.y2)*0.5f;
        const float ccx=(candidate.x1+candidate.x2)*0.5f, ccy=(candidate.y1+candidate.y2)*0.5f;
        const float distance=std::hypot(ccx-pcx, ccy-pcy);
        if (distance <= max_delta || distance <= 0.001f) return candidate;
        const float scale=max_delta/distance, cx=pcx+(ccx-pcx)*scale, cy=pcy+(ccy-pcy)*scale;
        const float w=candidate.x2-candidate.x1, h=candidate.y2-candidate.y1;
        candidate.x1=cx-w*0.5f; candidate.x2=cx+w*0.5f;
        candidate.y1=cy-h*0.5f; candidate.y2=cy+h*0.5f;
        return candidate;
    }
    void publish_snapshot(uint64_t frame_id, int64_t pts_ms, std::vector<Detection> detections) {
        if (!config_.align_to_video_pts) return;
        snapshots_.push_back({frame_id, pts_ms, std::move(detections)});
        while (snapshots_.size() > 12) snapshots_.pop_front();
    }
    void log_update(const Track& t, uint64_t frame_id, int64_t pts, float dt,
                    const Detection& raw, const Detection& smooth, float overlap, float distance,
                    bool deadzone, bool rebuilt, bool innovation, bool innovation_confirmed) const {
        if (!config_.debug_logging) return;
        std::cout << "[TrackUpdate] id="<<t.id<<" frame="<<frame_id<<" detection_pts="<<pts<<" current_pts="<<pts<<" dt_s="<<dt
                  <<" raw="<<raw.x1<<","<<raw.y1<<","<<raw.x2<<","<<raw.y2<<" smooth="<<smooth.x1<<","<<smooth.y1<<","<<smooth.x2<<","<<smooth.y2
                  <<" display="<<smooth.x1<<","<<smooth.y1<<","<<smooth.x2<<","<<smooth.y2
                  <<" iou="<<overlap<<" center_distance="<<distance
                  <<" center_motion="<<t.filtered_center_motion<<" size_motion="<<t.filtered_size_motion
                  <<" center_alpha="<<t.last_center_alpha<<" size_alpha="<<t.last_size_alpha
                  <<" reversal_damped="<<t.last_reversal_damped
                  <<" deadzone="<<deadzone<<" rebuilt="<<rebuilt<<" innovation="<<innovation
                  <<" innovation_confirmed="<<innovation_confirmed<<" pending_hits="<<t.pending_hits
                  <<"\n"<<std::flush;
    }
    void log_motion_track(const Track& track, uint64_t frame_id, int64_t pts_ms, float dt_sec,
                          const Detection& raw, bool deadzone, bool filter_reset,
                          bool was_active) const {
        if (!config_.motion_stats_logging) return;
        std::ostringstream line;
        line << std::setprecision(9)
             << "[MotionTrack] frame=" << frame_id
             << " pts_ms=" << pts_ms
             << " id=" << track.id
             << " class_id=" << raw.class_id
             << " dt_ms=" << static_cast<int64_t>(std::llround(dt_sec * 1000.0f))
             << " raw=" << raw.x1 << ',' << raw.y1 << ',' << raw.x2 << ',' << raw.y2
             << " smooth=" << track.smooth.x1 << ',' << track.smooth.y1 << ','
             << track.smooth.x2 << ',' << track.smooth.y2
             << " deadzone=" << deadzone
             << " reset=" << filter_reset
             << " was_active=" << was_active
             << " active=" << (track.state == TrackState::Active)
             << " center_alpha=" << track.last_center_alpha
             << " size_alpha=" << track.last_size_alpha;
        line << " effective_size_alpha_max=" << track.last_effective_size_alpha_max;
        line << " global_motion_used=" << track.last_global_motion_used
             << " global_motion_dx=" << track.last_global_motion_dx
             << " global_motion_dy=" << track.last_global_motion_dy;
        std::cout << line.str() << '\n';
    }
    mutable std::mutex mutex_; TrackerConfig config_; std::vector<Track> tracks_; std::vector<Detection> last_raw_; std::deque<Snapshot> snapshots_;
    uint64_t next_track_id_=1,last_detection_frame_id_=0; int64_t last_detection_pts_ms_=0; size_t last_matched_=0,last_created_=0,last_lost_=0;
    uint64_t total_reversal_damped_=0;
    std::vector<MotionSample> previous_motion_samples_;
    int64_t previous_motion_pts_ms_=0;
    bool global_motion_valid_=false;
    float filtered_global_motion_dx_=0.0f,filtered_global_motion_dy_=0.0f;
    int64_t global_motion_pts_ms_=0;
};
