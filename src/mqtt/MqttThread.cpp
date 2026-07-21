#include "mqtt/MqttThread.hpp"
#include <iostream>

MqttThread::MqttThread(const MqttConfig& cfg, BlockingQueue<std::string>& in_queue)
    : cfg_(cfg), in_queue_(in_queue) {}

MqttThread::~MqttThread() { stop(); }

void MqttThread::start() {
    running_ = true;
    thread_ = std::thread(&MqttThread::run, this);
}

void MqttThread::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void MqttThread::run() {
    std::string server_uri = "tcp://" + cfg_.broker + ":" + std::to_string(cfg_.port);
    mqtt::async_client client(server_uri, cfg_.client_id);

    mqtt::connect_options opts;
    // All three setters are inline in paho-mqtt-cpp 1.2.0 — no library symbol needed.
    opts.set_keep_alive_interval(20);
    opts.set_clean_session(true);
    opts.set_automatic_reconnect(true);

    try {
        client.connect(opts)->wait();
        std::cout << "[channel=" << cfg_.channel_id << "] MqttThread: connected to "
                  << server_uri << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[channel=" << cfg_.channel_id << "] MqttThread: connect failed: "
                  << e.what() << "\n";
        return;
    }

    while (running_) {
        std::string payload;
        if (!in_queue_.pop(payload, 500)) continue;

        try {
            client.publish(cfg_.topic, payload, 0, false)->wait();
        } catch (const std::exception& e) {
            std::cerr << "[channel=" << cfg_.channel_id << "] MqttThread: publish failed: "
                      << e.what() << "\n";
        }
    }

    try { client.disconnect()->wait(); } catch (...) {}
}
