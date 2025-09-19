#pragma once
#include "market/market_types.h"
#include <atomic>
#include <thread>

class UdpFeed {
public:
    UdpFeed(int port, TickCallback cb);
    ~UdpFeed();

    bool start();
    void stop();

private:
    void loop();

    int port_{0};
    int sock_{-1};
    TickCallback cb_;
    std::atomic<bool> running_;
    std::thread thread_;
};
