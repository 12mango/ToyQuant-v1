#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "common/market_event.h"
#include "concurrency/spsc_ring.h"
#include "market/market_data_adapter.h"

/*
-------------------------------------------
 WebSocketFeed
-------------------------------------------

The live half of the seam the roadmap names: a market data reader where the bytes arrive over a
socket instead of from a file. It is a client for the WebSocket protocol as an RFC 6455 handshake
plus text frames, written against the same interface every replay reader implements, so the book,
the strategy, the matching engine, the recorder and the paper broker cannot tell a live session
from a replayed one.

What it carries is the pair of streams the replay formats already use, one CSV row per text
message, in exactly the shapes `MarketEventRecorder` writes and the Deribit readers read:

    exchange,symbol,timestamp,id,side,price,amount                                   (a trade)
    exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount          (a depth change)

The kind of a row is its column count, so a `_trades.csv` and a `_depth.csv` can be interleaved on
the wire with no envelope and no transformation, and a recording made by `--record` streams back
over this feed unchanged.

Two readers, one connection. The receive thread fans every parsed row into a trade queue or a depth
queue, and each reader drains its own queue; `L2ReplayFeed` then merges the two by timestamp exactly
as it merges two files. Depth rows that share a local timestamp are regrouped into one
`IncrementalBookBatch` by the depth reader, which is the same rule the incremental CSV reader groups
on, so the book applies a live batch the way it applies a replayed one.

A reader returns false once the session has ended and its own queue is empty. The session ends when
the peer closes the connection, the handshake is refused, or `stop()` is called; a reader that is
still waiting then unblocks instead of spinning forever.
*/

// What a session carried. Every counter is written by the receive thread and read after the thread
// has been joined, which is why none of them needs an atomic of its own.
struct WebSocketFeedSummary
{
    bool connected{false};
    bool clean_close{false};
    uint64_t frames{0};
    uint64_t messages{0};
    uint64_t trades{0};
    uint64_t depth_updates{0};
    uint64_t control_frames{0};
    uint64_t parse_errors{0};
    // True when the session ended on its own --live-seconds rather than because the peer closed; the
    // two are different events and a run wants to know which one it got.
    bool stopped_by_deadline{false};

    std::string to_log_string() const;
};

class WebSocketFeed
{
   public:
    // A ws:// URL is these three pieces. host is a literal address or a name the resolver knows,
    // target is the HTTP path the handshake asks for ("/" when empty).
    struct Endpoint
    {
        std::string host{"127.0.0.1"};
        uint16_t port{0};
        std::string target{"/"};
    };

    static constexpr std::size_t kDefaultRingCapacity = 4096;

    // Everything that is not the address. A struct rather than more constructor arguments, because a
    // feed that gains a knob should not change every call site, and the three defaults are what both a
    // desk loop and a test want.
    struct Options
    {
        std::size_t ring_capacity{kDefaultRingCapacity};
        // Wall-clock seconds to stream before the session stops on its own; 0 runs until the peer
        // closes the connection. It is what a real capture needs, where nobody closes the socket for
        // you and the run has to end some other way. What has already arrived is still drained, so a
        // stop does not drop an event a reader has not seen.
        double live_seconds{0.0};
        // The wall-clock budget for the TCP connect and the opening handshake, in milliseconds; 0
        // waits indefinitely. A host that accepts the connection and then says nothing -- a filtered
        // port, a wedged peer -- would otherwise hang the run rather than fail it.
        int64_t connect_timeout_ms{5000};
    };

    // Two overloads rather than one with a default argument: a default argument to a nested aggregate
    // cannot use that aggregate's own default member initializers inside the class body, so the
    // one-argument form is spelled out and delegates instead.
    explicit WebSocketFeed(Endpoint endpoint);
    WebSocketFeed(Endpoint endpoint, Options options);

    // Stops the receive thread and closes the socket. The destructor calls it, so a feed that goes
    // out of scope releases its socket instead of terminating on a joinable thread.
    ~WebSocketFeed();

    WebSocketFeed(const WebSocketFeed&) = delete;
    WebSocketFeed& operator=(const WebSocketFeed&) = delete;

    // Connects and performs the opening handshake on the calling thread, so a refusal is thrown here
    // rather than surfacing inside a reader. Then it starts the receive thread.
    void start();

    // Ends the session and joins the receive thread. Idempotent, so a caller can stop a feed that has
    // already ended without checking. A reader that was waiting unblocks.
    void stop();

    // The two readers the L2 delivery path takes. Each owns nothing but a reference to this feed, so
    // both have to be used (and this feed kept alive) while they are.
    std::unique_ptr<IMarketEventReader> trades_reader();
    std::unique_ptr<IMarketEventReader> depth_reader();

    // The Sec-WebSocket-Accept value for a client key: base64(SHA1(key + the RFC 6455 GUID)). It is
    // exposed so a test can stand in for a server and complete the handshake the client verifies,
    // against the same hash rather than a second copy of it.
    static std::string accept_for_key(std::string_view key);

    const WebSocketFeedSummary& summary() const
    {
        return summary_;
    }

   private:
    // One depth row, which carries its symbol and exchange as well: an IncrementalBookUpdate names
    // neither, and the reader needs both to build the batch the book applies.
    struct DepthMessage
    {
        std::string symbol;
        std::string exchange;
        IncrementalBookUpdate update;
    };

    // The two IMarketEventReader implementations. They are nested so they can reach the feed's queues
    // and run flags without widening the public surface with accessors that exist only for them.
    class TradeReader;
    class DepthReader;

    void receive_loop();
    bool connect_socket();
    void perform_handshake();

    // Reads exactly `count` bytes, returning false at end of input, once stop() has been asked for, or
    // once `deadline_ms` (milliseconds on the monotonic clock, 0 for none) has passed. The socket
    // carries a receive timeout so a waiting read can notice any of the three rather than block a join;
    // the deadline has to live here as well as in the caller, because a silent peer leaves the loop
    // inside this function and a check between frames would never run.
    bool read_exact(void* destination, std::size_t count, int64_t deadline_ms = 0);
    // One WebSocket frame. Returns false at end of input or on the deadline. A control frame is
    // reported through `opcode` and answered by the caller; a data frame's payload goes to `payload`.
    bool read_frame(std::string& payload, int& opcode, bool& fin, int64_t deadline_ms = 0);
    void handle_message(std::string_view message);

    Endpoint endpoint_;
    Options options_;
    int sock_{-1};
    // Set true by start(), false by stop(). A reader that sees it clear gives up waiting.
    std::atomic<bool> running_{false};
    // Set true, with release, by the receive thread immediately before it returns. A reader that sees
    // it set knows every push that will ever happen has already happened.
    std::atomic<bool> producer_done_{false};
    std::thread thread_;

    concurrency::SpscRing<MarketTrade> trades_;
    concurrency::SpscRing<DepthMessage> depth_;
    WebSocketFeedSummary summary_;
};
