#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mqtt/async_client.h>
#include "common/BlockingQueue.hpp"

struct MqttConfig {
    std::string broker;
    int         port;
    std::string topic;
    std::string client_id;
};

class MqttThread {
public:
    MqttThread(const MqttConfig& cfg, BlockingQueue<std::string>& in_queue);
    ~MqttThread();

    void start();
    void stop();

private:
    void run();

    MqttConfig                  cfg_;
    BlockingQueue<std::string>& in_queue_;
    std::thread                 thread_;
    std::atomic<bool>           running_{false};
};
