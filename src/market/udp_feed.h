#pragma once
#include "market/market_types.h"
#include <atomic>
#include <thread>
#include <vector>

class UdpFeed {
public:
    explicit UdpFeed(int port);

    bool start();
    void stop();

    bool pop_tick(Tick& t);

    uint64_t get_tick_count() const { return tick_count_.load(std::memory_order_relaxed); }
    size_t unread_count() const;

private:
    void loop();

    // C++ 风格解析（string_view + find）
    bool parse_tick_cpp(std::string_view s, Tick& t);

    // static const
    static constexpr size_t RING_SIZE = 8192;
    static constexpr size_t MAX_PKT = 2048;
    static constexpr unsigned BATCH = 8;

    int port_{0};
    int sock_{-1};

    std::atomic<bool> running_{false};
    std::thread thread_;

    std::vector<Tick> ring_{std::vector<Tick>(RING_SIZE)};
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};

    std::atomic<uint64_t> tick_count_{0};

    std::vector<std::array<char, MAX_PKT>> recv_bufs_;
};
