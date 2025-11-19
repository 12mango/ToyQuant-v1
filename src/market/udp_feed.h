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

    int get_tick_count() const { return tick_count_.load(); }  // 接收 tick 数量接口

private:
    void loop();

    int port_{0};
    int sock_{-1};
    TickCallback cb_;
    std::atomic<bool> running_;
    std::thread thread_;
    std::atomic<int> tick_count_{0};  // tick 计数
};
