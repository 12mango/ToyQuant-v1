#include "market/udp_feed.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <iostream>

UdpFeed::UdpFeed(int port, TickCallback cb)
    : port_(port), cb_(cb), running_(false) {}

UdpFeed::~UdpFeed() {
    stop();
}

bool UdpFeed::start() {
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) return false;

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) < 0) return false;

    running_.store(true);
    thread_ = std::thread([this]() { this->loop(); });
    return true;
}

void UdpFeed::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
}

void UdpFeed::loop() {
    char buf[1024];
    while (running_.load()) {
        sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(sock_, buf, sizeof(buf) - 1, 0, (sockaddr*)&src, &slen);
        if (n <= 0) continue;

        buf[n] = 0;
        std::string s(buf);

        Tick t;
        std::istringstream ss(s);
        std::string tok;
        if (!std::getline(ss, tok, ',')) continue;
        t.ts = std::stoull(tok);
        if (!std::getline(ss, t.symbol, ',')) continue;
        if (!std::getline(ss, tok, ',')) continue;
        t.price = std::stod(tok);
        if (!std::getline(ss, tok, ',')) continue;
        t.size = std::stoull(tok);
        if (!std::getline(ss, tok, ',')) continue;
        t.side = tok.empty() ? 'N' : tok[0];

        tick_count_++;  // 累计 tick
        cb_(t);         // 原回调
    }
}
