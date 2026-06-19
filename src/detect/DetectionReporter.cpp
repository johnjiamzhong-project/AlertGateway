#include "detect/DetectionReporter.hpp"
#include <nlohmann/json.hpp>
#include <ctime>
#include <map>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstring>

using json = nlohmann::json;

DetectionReporter::DetectionReporter(const DetectionConfig& cfg,
                                     BlockingQueue<Frame>& in_queue,
                                     BlockingQueue<Frame>& encode_queue,
                                     BlockingQueue<std::string>& mqtt_queue)
    : cfg_(cfg), in_queue_(in_queue), encode_queue_(encode_queue), mqtt_queue_(mqtt_queue) {}

DetectionReporter::~DetectionReporter() { stop(); }

void DetectionReporter::start() {
    last_report_time_ = std::chrono::steady_clock::now();
    running_ = true;
    thread_ = std::thread(&DetectionReporter::run, this);
}

void DetectionReporter::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void DetectionReporter::run() {
    while (running_) {
        Frame frame;
        if (!in_queue_.pop(frame, 200)) continue;

        draw_detections(frame);
        encode_queue_.push(frame, 200);

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_report_time_).count();

        if (elapsed >= cfg_.report_interval_sec) {
            last_report_time_ = now;
            std::string summary = summarize(frame.detections);
            if (summary != last_summary_) {
                last_summary_ = summary;
                mqtt_queue_.push(summary, 100);
            }
        }
    }
}

static void draw_hline(uint8_t* rgb, int w, int h, int x0, int x1, int y,
                        uint8_t r, uint8_t g, uint8_t b) {
    if (y < 0 || y >= h) return;
    x0 = std::max(x0, 0); x1 = std::min(x1, w - 1);
    for (int x = x0; x <= x1; x++) {
        uint8_t* p = rgb + (y * w + x) * 3;
        p[0] = r; p[1] = g; p[2] = b;
    }
}

static void draw_vline(uint8_t* rgb, int w, int h, int x, int y0, int y1,
                        uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= w) return;
    y0 = std::max(y0, 0); y1 = std::min(y1, h - 1);
    for (int y = y0; y <= y1; y++) {
        uint8_t* p = rgb + (y * w + x) * 3;
        p[0] = r; p[1] = g; p[2] = b;
    }
}

static void draw_rect(uint8_t* rgb, int w, int h,
                       int x1, int y1, int x2, int y2, int thickness,
                       uint8_t r, uint8_t g, uint8_t b) {
    for (int t = 0; t < thickness; t++) {
        draw_hline(rgb, w, h, x1, x2, y1 + t, r, g, b);
        draw_hline(rgb, w, h, x1, x2, y2 - t, r, g, b);
        draw_vline(rgb, w, h, x1 + t, y1, y2, r, g, b);
        draw_vline(rgb, w, h, x2 - t, y1, y2, r, g, b);
    }
}

void DetectionReporter::draw_detections(Frame& frame) {
    if (frame.rgb_data.empty()) return;
    uint8_t* rgb = frame.rgb_data.data();
    int w = frame.width, h = frame.height;

    for (const auto& det : frame.detections) {
        draw_rect(rgb, w, h,
                  static_cast<int>(det.x1), static_cast<int>(det.y1),
                  static_cast<int>(det.x2), static_cast<int>(det.y2),
                  2, 0, 255, 0);
    }
}

std::string DetectionReporter::summarize(const std::vector<Detection>& dets) {
    std::map<std::string, std::pair<int, float>> tally;
    for (const auto& d : dets) {
        auto& entry = tally[d.label];
        entry.first++;
        entry.second = std::max(entry.second, d.score);
    }

    json objects = json::array();
    for (const auto& [label, cv] : tally) {
        objects.push_back({
            {"label", label},
            {"count", cv.first},
            {"score", std::round(cv.second * 100.0f) / 100.0f}
        });
    }

    json msg;
    msg["timestamp"] = static_cast<int64_t>(std::time(nullptr));
    msg["objects"]   = objects;
    return msg.dump();
}
