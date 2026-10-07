// The websocket feed's contract is the seam the roadmap names: a live session is a replay with the
// readers swapped, so what arrives over a socket has to become exactly the MarketEvent stream a file
// would have produced. This test stands a WebSocket server in for a venue -- the same handshake, the
// same frames -- and checks that the two readers reconstruct the trades and the depth batches the
// rows describe, including the regrouping of depth rows by their local timestamp.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

#include "market/ws_feed.h"

namespace
{
// One unmasked server-to-client WebSocket frame; a server never masks, and Opcode::Close carries the
// end of the session for the client's readers.
std::string frame(std::string_view payload, int opcode = 0x1)
{
    std::string out;
    out.push_back(static_cast<char>(0x80 | opcode));
    const std::size_t length = payload.size();
    if (length < 126)
        out.push_back(static_cast<char>(length));
    else if (length < 65536)
    {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>(length >> 8));
        out.push_back(static_cast<char>(length & 0xFF));
    }
    else
    {
        out.push_back(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8)
            out.push_back(static_cast<char>((static_cast<uint64_t>(length) >> shift) & 0xFF));
    }
    out.append(payload);
    return out;
}

std::string read_http_request(int fd)
{
    std::string request;
    char byte = 0;
    while (request.find("\r\n\r\n") == std::string::npos)
    {
        const ssize_t got = ::recv(fd, &byte, 1, 0);
        if (got <= 0) break;
        request.push_back(byte);
    }
    return request;
}

std::string header(std::string_view request, std::string_view name)
{
    std::size_t start = request.find("\r\n");
    while (start != std::string_view::npos)
    {
        start += 2;
        const std::size_t end = request.find("\r\n", start);
        if (end == std::string_view::npos) break;
        std::string_view line = request.substr(start, end - start);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos)
        {
            std::string_view key = line.substr(0, colon);
            bool match = key.size() == name.size();
            for (std::size_t i = 0; match && i < key.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(key[i])) != name[i]) match = false;
            if (match)
            {
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
                return std::string(value);
            }
        }
        start = end;
    }
    return {};
}

void send_all(int fd, std::string_view data)
{
    std::size_t done = 0;
    while (done < data.size())
    {
        const ssize_t sent = ::send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (sent <= 0) return;
        done += static_cast<std::size_t>(sent);
    }
}

// A one-connection WebSocket server: handshake, then one text frame per row, then a close frame.
// It plays the venue so the client is exercised against real protocol bytes rather than a stub.
class LoopbackServer
{
   public:
    explicit LoopbackServer(std::vector<std::string> rows) : rows_(std::move(rows))
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(listener_ >= 0);
        int option = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;  // let the kernel choose, so parallel runs never collide
        assert(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t length = sizeof(address);
        assert(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        assert(listen(listener_, 1) == 0);
    }

    ~LoopbackServer()
    {
        if (thread_.joinable()) thread_.join();
        if (listener_ >= 0) ::close(listener_);
    }

    uint16_t port() const { return port_; }

    void serve()
    {
        thread_ = std::thread([this] { run(); });
    }

   private:
    void run()
    {
        const int conn = ::accept(listener_, nullptr, nullptr);
        if (conn < 0) return;
        const std::string request = read_http_request(conn);
        const std::string key = header(request, "sec-websocket-key");
        assert(!key.empty());
        const std::string accept = WebSocketFeed::accept_for_key(key);
        const std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                                     "Upgrade: websocket\r\n"
                                     "Connection: Upgrade\r\n"
                                     "Sec-WebSocket-Accept: " +
                                     accept + "\r\n\r\n";
        send_all(conn, response);
        for (const std::string& row : rows_) send_all(conn, frame(row + "\n"));
        send_all(conn, frame("", 0x8));
        ::shutdown(conn, SHUT_WR);
        ::close(conn);
    }

    int listener_{-1};
    uint16_t port_{0};
    std::vector<std::string> rows_;
    std::thread thread_;
};
}  // namespace

int main()
{
    // Rows in the two replay shapes, each file's header first. Two depth rows that share a local
    // timestamp are separated on the wire by a trade, which is the case the depth reader's regrouping
    // has to survive: the trade goes to the other queue, so the pair stays adjacent in depth order.
    std::vector<std::string> rows = {
        "exchange,symbol,timestamp,id,side,price,amount",
        "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount",
        "ws,ETH-PERPETUAL,1000,1,buy,100.5,3",
        "ws,ETH-PERPETUAL,1000,1000,0,bid,100.4,5",
        "ws,ETH-PERPETUAL,1500,2,sell,100.4,2",
        "ws,ETH-PERPETUAL,1000,1000,0,ask,100.6,7",
        "ws,ETH-PERPETUAL,2000,2000,0,bid,100.5,3",
    };
    LoopbackServer server(rows);
    server.serve();

    WebSocketFeed feed(WebSocketFeed::Endpoint{"127.0.0.1", server.port(), "/"});
    feed.start();
    auto trades_reader = feed.trades_reader();
    auto depth_reader = feed.depth_reader();

    std::vector<MarketTrade> trades;
    MarketEvent event;
    while (trades_reader->next(event))
    {
        assert(std::holds_alternative<MarketTrade>(event));
        trades.push_back(std::get<MarketTrade>(event));
    }

    std::vector<IncrementalBookBatch> batches;
    while (depth_reader->next(event))
    {
        assert(std::holds_alternative<IncrementalBookBatch>(event));
        batches.push_back(std::get<IncrementalBookBatch>(event));
    }
    feed.stop();

    // The two header rows were skipped, so the counters are rows minus headers.
    assert(trades.size() == 2);
    assert(trades[0].exchange == "ws");
    assert(trades[0].symbol == "ETH-PERPETUAL");
    assert(trades[0].ts == 1000);
    assert(trades[0].sequence == 1);
    assert(trades[0].aggressor_side == Side::Buy);
    assert(trades[0].price == 100.5);
    assert(trades[0].quantity == 3);
    assert(trades[1].ts == 1500);
    assert(trades[1].aggressor_side == Side::Sell);
    assert(trades[1].quantity == 2);

    // Three depth rows became two batches: the two that share local_ts 1000, then the one at 2000.
    assert(batches.size() == 2);
    assert(batches[0].symbol == "ETH-PERPETUAL");
    assert(batches[0].exchange == "ws");
    assert(batches[0].exchange_ts == 1000);
    assert(batches[0].local_ts == 1000);
    assert(batches[0].updates.size() == 2);
    assert(batches[0].updates[0].side == Side::Buy);
    assert(batches[0].updates[0].price == 100.4);
    assert(batches[0].updates[0].amount == 5);
    assert(batches[0].updates[1].side == Side::Sell);
    assert(batches[0].updates[1].price == 100.6);
    assert(batches[0].updates[1].amount == 7);
    assert(batches[0].has_snapshot() == false);
    assert(batches[1].local_ts == 2000);
    assert(batches[1].updates.size() == 1);

    const WebSocketFeedSummary& summary = feed.summary();
    assert(summary.connected);
    assert(summary.clean_close);
    assert(summary.messages == rows.size());
    assert(summary.trades == 2);
    assert(summary.depth_updates == 3);
    assert(summary.parse_errors == 0);

    return 0;
}
