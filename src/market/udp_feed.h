#pragma once
#include <atomic>
#include <thread>
#include <vector>

#include "legacy/tick.h"

class UdpFeed
{
   public:
    explicit UdpFeed(int port);

    // Stops the receive thread and closes the socket. The destructor calls it, so a feed that
    // goes out of scope releases its socket instead of terminating on a joinable thread.
    ~UdpFeed();

    bool start();
    void stop();

    bool pop_tick(legacy::Tick& t);

   private:
    void loop();

    // C++ parser using string_view and find.
    bool parse_tick_cpp(std::string_view s, legacy::Tick& t);

    // static const
    static constexpr size_t RING_SIZE = 8192;
    static constexpr size_t MAX_PKT = 2048;
    static constexpr unsigned BATCH = 8;

    int port_{0};
    int sock_{-1};

    std::atomic<bool> running_{false};
    std::thread thread_;

    std::vector<legacy::Tick> ring_{std::vector<legacy::Tick>(RING_SIZE)};
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};

    std::vector<std::array<char, MAX_PKT>> recv_bufs_;
};
