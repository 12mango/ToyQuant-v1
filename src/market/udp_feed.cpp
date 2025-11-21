#include "market/udp_feed.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <iostream>
#include <fcntl.h>
#include <immintrin.h>

UdpFeed::UdpFeed(int port) : port_(port) {}

bool UdpFeed::start() {
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) return false;

    // 设置非阻塞
    int flags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) < 0)
        return false;

    running_.store(true);
    thread_ = std::thread([this]() { loop(); });
    return true;
}

void UdpFeed::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (sock_ >= 0) close(sock_);
}

bool UdpFeed::parse_tick(const std::string& s, Tick& t) {
    std::istringstream ss(s);
    std::string tok;

    if (!std::getline(ss, tok, ',')) return false;
    t.ts = std::stoull(tok);
    if (!std::getline(ss, t.symbol, ',')) return false;
    if (!std::getline(ss, tok, ',')) return false;
    t.price = std::stod(tok);
    if (!std::getline(ss, tok, ',')) return false;
    t.size = std::stoull(tok);
    if (!std::getline(ss, tok, ',')) return false;
    t.side = tok.empty() ? 'N' : tok[0];

    return true;
}

void UdpFeed::loop() {
    char buf[1024];

    while (running_.load(std::memory_order_acquire)) {
        sockaddr_in src{};
        socklen_t slen = sizeof(src);

        ssize_t n = recvfrom(sock_, buf, sizeof(buf) - 1, 0, (sockaddr*)&src, &slen);
        if (n > 0) {
            buf[n] = 0;
            Tick t;
            if (!parse_tick(buf, t)) continue;

            size_t h = head_.load(std::memory_order_relaxed);
            size_t next = (h + 1) % RING_SIZE;

            // 不覆盖未读数据
            if (next != tail_.load(std::memory_order_acquire)) {
                ring_[h] = t;
                head_.store(next, std::memory_order_release);
                tick_count_.fetch_add(1, std::memory_order_relaxed);
            }
            // else 丢弃 tick
        } else {
            // 没有数据时，轻量等待，减少 CPU 占用
            _mm_pause();
        }
    }
}

bool UdpFeed::pop_tick(Tick& t) {
    size_t tl = tail_.load(std::memory_order_relaxed);
    size_t hd = head_.load(std::memory_order_acquire);

    if (tl == hd) return false;

    t = ring_[tl];
    tail_.store((tl + 1) % RING_SIZE, std::memory_order_release);
    return true;
}

size_t UdpFeed::unread_count() const {
    size_t h = head_.load(std::memory_order_acquire);
    size_t t = tail_.load(std::memory_order_acquire);
    return (h >= t) ? (h - t) : (RING_SIZE - t + h);
}
