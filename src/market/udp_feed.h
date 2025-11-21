#pragma once
#include "market/market_types.h"
#include <atomic>
#include <thread>
#include <vector>
#include <cstddef>

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
    bool parse_tick(const std::string& s, Tick& t);

    static constexpr size_t RING_SIZE = 8192;

    int port_{0};
    int sock_{-1};

    std::atomic<bool> running_{false};
    std::thread thread_;

    std::vector<Tick> ring_{std::vector<Tick>(RING_SIZE)};
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};

    std::atomic<uint64_t> tick_count_{0};
};
