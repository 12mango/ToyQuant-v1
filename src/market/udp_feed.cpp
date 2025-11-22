#include "market/udp_feed.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <string_view>
#include <immintrin.h>
#include <sys/uio.h>

UdpFeed::UdpFeed(int port)
    : port_(port), recv_bufs_(BATCH) {}

static inline void set_sock_rcvbuf(int fd, int mb) {
    int val = mb * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val));
}

bool UdpFeed::start() {
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) return false;

    set_sock_rcvbuf(sock_, 4);  // 调大接收缓存

    int flags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    int opt = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock_);
        return false;
    }

    running_.store(true);
    thread_ = std::thread([this]() { loop(); });
    return true;
}

void UdpFeed::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (sock_ >= 0) close(sock_);
    sock_ = -1;
}

/*
-------------------------------------------
 C++ 风格解析（推荐用于 C++ 工程）
-------------------------------------------

格式: "ts,symbol,price,size,side"

使用 string_view & find()，零拷贝，性能很好。
-------------------------------------------

 同时保留下面 C 风格写法的示例（面试可讲）：

 // C version (not used)
 const char* p = buf;
 char* end;
 t.ts = strtoull(p, &end, 10);
 p = end + 1;

-------------------------------------------
*/

bool UdpFeed::parse_tick_cpp(std::string_view s, Tick& t) {
    size_t pos = 0, end;

    // ts
    end = s.find(',', pos);
    if (end == std::string_view::npos) return false;
    t.ts = std::stoull(std::string(s.substr(pos, end - pos)));
    pos = end + 1;

    // symbol
    end = s.find(',', pos);
    if (end == std::string_view::npos) return false;
    t.symbol = std::string(s.substr(pos, end - pos));
    pos = end + 1;

    // price
    end = s.find(',', pos);
    if (end == std::string_view::npos) return false;
    t.price = std::stod(std::string(s.substr(pos, end - pos)));
    pos = end + 1;

    // size
    end = s.find(',', pos);
    if (end == std::string_view::npos) return false;
    t.size = std::stoull(std::string(s.substr(pos, end - pos)));
    pos = end + 1;

    // side
    if (pos >= s.size()) return false;
    t.side = s[pos];

    return true;
}

void UdpFeed::loop() {
#if defined(__linux__)
    struct mmsghdr msgs[BATCH];
    struct iovec iovs[BATCH];
    struct sockaddr_in addrs[BATCH];
    memset(msgs, 0, sizeof(msgs));

    for (unsigned i = 0; i < BATCH; ++i) {
        iovs[i].iov_base = recv_bufs_[i].data();
        iovs[i].iov_len = recv_bufs_[i].size();
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &addrs[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
    }

    while (running_.load()) {
        int ret = recvmmsg(sock_, msgs, BATCH, 0, nullptr);
        if (ret < 0) {
            if (errno == EAGAIN) { _mm_pause(); continue; }
            _mm_pause(); continue;
        }

        for (int i = 0; i < ret; ++i) {
            int len = msgs[i].msg_len;
            if (len <= 0) continue;

            std::string_view sv(recv_bufs_[i].data(), len);

            Tick t;
            if (!parse_tick_cpp(sv, t)) continue;

            size_t h = head_.load(std::memory_order_relaxed);
            size_t next = (h + 1) % RING_SIZE;

            if (next != tail_.load(std::memory_order_acquire)) {
                ring_[h] = t;
                head_.store(next, std::memory_order_release);
                tick_count_.fetch_add(1);
            }
        }
    }
#else
    char buf[MAX_PKT];

    while (running_.load()) {
        sockaddr_in src{};
        socklen_t slen = sizeof(src);

        ssize_t n = recvfrom(sock_, buf, sizeof(buf), 0, (sockaddr*)&src, &slen);
        if (n <= 0) {
            if (errno == EAGAIN) { _mm_pause(); continue; }
            _mm_pause(); continue;
        }

        std::string_view sv(buf, n);
        Tick t;
        if (!parse_tick_cpp(sv, t)) continue;

        size_t h = head_.load(std::memory_order_relaxed);
        size_t next = (h + 1) % RING_SIZE;

        if (next != tail_.load(std::memory_order_acquire)) {
            ring_[h] = t;
            head_.store(next, std::memory_order_release);
            tick_count_.fetch_add(1);
        }
    }
#endif
}

bool UdpFeed::pop_tick(Tick& t) {
    size_t tl = tail_.load();
    size_t hd = head_.load();

    if (tl == hd) return false;

    t = ring_[tl];
    tail_.store((tl + 1) % RING_SIZE);
    return true;
}

size_t UdpFeed::unread_count() const {
    size_t h = head_.load();
    size_t t = tail_.load();
    return (h >= t) ? (h - t) : (RING_SIZE - t + h);
}
