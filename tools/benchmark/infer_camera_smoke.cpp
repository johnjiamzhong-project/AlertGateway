#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "capture/CaptureThread.hpp"
#include "common/BlockingQueue.hpp"
#include "common/Frame.hpp"
#include "common/SharedDetections.hpp"
#include "infer/InferThread.hpp"

using json = nlohmann::json;

namespace {

int parse_duration(const char* value) {
    size_t consumed = 0;
    int duration = 0;
    try {
        duration = std::stoi(value, &consumed);
    } catch (...) {
        throw std::runtime_error("duration must be an integer");
    }
    if (value[consumed] != '\0' || duration <= 0)
        throw std::runtime_error("duration must be a positive integer");
    return duration;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0] << " <config.json> [duration_seconds]\n";
        return 1;
    }

    try {
        int duration_seconds = argc == 3 ? parse_duration(argv[2]) : 60;
        std::ifstream config_file(argv[1]);
        if (!config_file) throw std::runtime_error("cannot open config");
        json cfg;
        config_file >> cfg;

        CameraConfig camera{
            cfg["camera"]["device"].get<std::string>(),
            cfg["camera"]["width"].get<int>(),
            cfg["camera"]["height"].get<int>(),
            cfg["camera"]["fps"].get<int>()
        };
        ModelConfig model{
            cfg["model"]["path"].get<std::string>(),
            cfg["model"]["conf_threshold"].get<float>(),
            cfg["model"]["iou_threshold"].get<float>(),
            cfg["detection"]["target_classes"].get<std::vector<std::string>>(),
            cfg["model"].value("infer_every_n_frames", 1),
            cfg["model"].value("output_layout", std::string("decoded"))
        };
        DetectionConfig detection{
            cfg["detection"].value("report_interval_sec", 1)
        };

        BlockingQueue<Frame> enc_queue(1);
        BlockingQueue<Frame> infer_queue(2);
        BlockingQueue<std::string> mqtt_queue(16);
        SharedDetections shared_detections;
        CaptureThread capture(camera, enc_queue, infer_queue);
        InferThread infer(model, detection, infer_queue, mqtt_queue,
                          shared_detections);

        std::atomic<bool> drain_running{true};
        std::thread drain_thread([&] {
            while (drain_running) {
                Frame frame;
                enc_queue.pop(frame, 50);
                std::string message;
                mqtt_queue.pop(message, 0);
            }
        });

        try {
            infer.start();
            capture.start();
        } catch (...) {
            capture.stop();
            infer.stop();
            drain_running = false;
            enc_queue.close();
            mqtt_queue.close();
            drain_thread.join();
            throw;
        }

        std::cout << "infer_camera_smoke running layout=" << model.output_layout
                  << " duration=" << duration_seconds << "s\n";
        for (int second = 1; second <= duration_seconds; ++second) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto detections = shared_detections.get();
            std::cout << "[Smoke] second=" << second
                      << " objects=" << detections.size();
            for (const auto& item : detections)
                std::cout << ' ' << item.label << ':' << item.score;
            std::cout << '\n' << std::flush;
        }

        capture.stop();
        infer.stop();
        drain_running = false;
        enc_queue.close();
        mqtt_queue.close();
        drain_thread.join();
        std::cout << "infer_camera_smoke completed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
