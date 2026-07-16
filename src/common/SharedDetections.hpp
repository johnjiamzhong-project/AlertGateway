#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
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
    float motion_full_response_ratio = 1.20f;
    float motion_smoothing_alpha = 0.35f;
    int display_hold_ms = 100;
    bool debug_logging = false;
    bool reversal_damping_enabled = false;
    float reversal_center_alpha_max = 0.35f;
    float reversal_min_motion_ratio = 0.005f;
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
                if (tracks_[ti].smooth.class_id != dets[di].class_id) continue;
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
                if (overlap < config_.match_iou && !distance_match) continue;
                const float active_bonus = tracks_[ti].state == TrackState::Active ? 0.05f : 0.0f;
                candidates.push_back({ti, di, overlap, distance,
                    overlap + size_similarity * 0.10f - distance / max_distance * 0.05f + active_bonus});
            }
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        std::vector<bool> matched_tracks(tracks_.size(), false), matched_dets(dets.size(), false);
        for (const Candidate& c : candidates) {
            if (matched_tracks[c.track] || matched_dets[c.detection]) continue;
            Track& track = tracks_[c.track];
            track.last_reversal_damped = false;
            const Detection prior_smooth = track.smooth;
            const float dt_sec = std::max(0.001f, static_cast<float>(pts_ms - track.last_detection_pts_ms) / 1000.0f);
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
            if (!deadzone) {
                // 中心和尺寸分开滤波。运动量按目标自身尺寸归一化，因此不依赖 4K 固定像素阈值。
                // 静止时中心/尺寸强平滑；快速运动时只让中心快速跟随，尺寸仍保持较强抑噪。
                filter_reset = track.state != TrackState::Active || c.overlap < config_.match_iou;
                if (filter_reset) {
                    smooth_measurement = dets[c.detection];
                    track.filtered_center_motion = 0.0f;
                    track.filtered_size_motion = 0.0f;
                    track.last_center_delta_valid = false;
                    center_alpha = size_alpha = 1.0f;
                } else if (config_.adaptive_filter) {
                    smooth_measurement = adaptive_box(track, prior_smooth, dets[c.detection], dt_sec,
                                                      &center_alpha, &size_alpha);
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
        std::vector<Detection> snapshot_detections;
        for (const Track& track : tracks_)
            if (is_displayable(track, pts_ms)) snapshot_detections.push_back(track.smooth);
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
    struct Track {
        uint64_t id;
        Detection smooth;
        int64_t last_detection_pts_ms;
        int hits, misses;
        Detection pending{};
        int pending_hits = 0;
        TrackState state = TrackState::Tentative;
        Detection last_measurement{};
        float filtered_center_motion = 0.0f;
        float filtered_size_motion = 0.0f;
        float last_center_alpha = 1.0f;
        float last_size_alpha = 1.0f;
        float last_center_dx = 0.0f;
        float last_center_dy = 0.0f;
        bool last_center_delta_valid = false;
        bool last_reversal_damped = false;
    };
    struct Snapshot { uint64_t frame_id; int64_t pts_ms; std::vector<Detection> detections; };
    struct Candidate { size_t track, detection; float overlap, distance, score; };
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
                           float dt_sec, float* center_alpha_out, float* size_alpha_out) {
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
        const float size_alpha = std::clamp(config_.size_alpha_min +
            (config_.size_alpha_max - config_.size_alpha_min) * size_response, 0.0f, 1.0f);
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

        const float cx = center_alpha * mcx + (1.0f - center_alpha) * pcx;
        const float cy = center_alpha * mcy + (1.0f - center_alpha) * pcy;
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
    mutable std::mutex mutex_; TrackerConfig config_; std::vector<Track> tracks_; std::vector<Detection> last_raw_; std::deque<Snapshot> snapshots_;
    uint64_t next_track_id_=1,last_detection_frame_id_=0; int64_t last_detection_pts_ms_=0; size_t last_matched_=0,last_created_=0,last_lost_=0;
    uint64_t total_reversal_damped_=0;
};
