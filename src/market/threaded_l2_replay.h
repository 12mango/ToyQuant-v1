#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "concurrency/spsc_ring.h"
#include "market/l2_replay_feed.h"
#include "market/market_data_adapter.h"
#include "market/market_data_validator.h"
#include "utils/latency_histogram.h"

// What the hand-off between the feed thread and the strategy thread costs, and what it looked like
// while it ran.
//
// There are three stamps per event and they answer three different questions. The ring wait is what a
// model loses by being on the far side of a queue. Processing is the strategy's own time: the decision
// plus whatever orders the engine accepted inside it, which is why there is no separate order leg
// here — the engine is synchronous on this path, and the venue's own leg is what --order-latency-us
// and --cancel-latency-us model. The total is what a live path budgets against, feed to done.
//
// The occupancy and the wait counters are what say whether the ring is the right shape. An occupancy
// that sits at the capacity means the strategy is the slow side and the feed is being throttled, and
// the pacing line is where that also shows up, as a feed that fell behind its own schedule.
struct LatencySummary
{
    // One per event that crossed the ring.
    uint64_t handoffs{0};
    int64_t queue_mean_ns{0};
    int64_t queue_p50_ns{0};
    int64_t queue_p99_ns{0};
    int64_t queue_max_ns{0};
    int64_t processing_mean_ns{0};
    int64_t processing_p50_ns{0};
    int64_t processing_p99_ns{0};
    int64_t processing_max_ns{0};
    int64_t total_mean_ns{0};
    int64_t total_p50_ns{0};
    int64_t total_p99_ns{0};
    int64_t total_max_ns{0};
    // The deepest the ring got, counted by the consumer after its own pop, so it includes the event
    // being processed: a value one above the ring's capacity means the ring was full and the strategy
    // never caught up.
    uint64_t ring_depth_max{0};
    // How often the feed found the ring full and had to wait for room, and how long it waited. A
    // replay must not lose an event, so a full ring is backpressure rather than a drop: the feed
    // waits, and these three fields are the price of that.
    uint64_t feed_waits{0};
    int64_t feed_wait_total_ns{0};
    int64_t feed_wait_max_ns{0};

    std::string to_log_string() const;
};

class ThreadedReplayFeed
{
   public:
    using EventCallback = L2ReplayFeed::EventCallback;
    static constexpr std::size_t kDefaultRingCapacity = 4096;

    ThreadedReplayFeed(std::unique_ptr<IMarketEventReader> trades,
                       std::unique_ptr<IMarketEventReader> depth, EventCallback consumer,
                       ReplayPacing pacing = {}, MarketDataValidationConfig validation_config = {},
                       std::size_t ring_capacity = kDefaultRingCapacity);

    // Runs the feed on a thread it starts and consumes on the calling thread, so the consumer
    // callback runs on the same thread it would have in the direct path and a caller never has to
    // think about which thread its own state is touched from. Returns after both threads are joined,
    // and rethrows a failure from either side rather than leaving a thread spinning.
    void run();

    // Optional per-stage timing, forwarded to the feed thread's own feed: the strategy stages are
    // timed on this thread, and the read and parse stages on the other one.
    void set_profiler(StageProfiler* profiler);

    const LatencySummary& latency_summary() const
    {
        return summary_;
    }

    const PacingSummary& pacing_summary() const
    {
        return feed_->pacing_summary();
    }

    const MarketDataValidationSummary& validation_summary() const
    {
        return feed_->validation_summary();
    }

   private:
    // One value on the ring. The stamp is taken when the feed hands the event over, which is what
    // makes the ring wait a measured quantity rather than a guess.
    struct StampedEvent
    {
        int64_t feed_ns{0};
        MarketEvent event;
    };

    void producer_loop();
    void produce(const MarketEvent& event);
    void consume_loop();

    EventCallback consumer_;
    // The ring is declared before the feed it is handed to, so it outlives it.
    std::unique_ptr<concurrency::SpscRing<StampedEvent>> ring_;
    std::unique_ptr<L2ReplayFeed> feed_;
    // Set by the producer after its feed has finished, and by either side when the run has to stop.
    std::atomic<bool> producer_done_{false};
    std::atomic<bool> abort_{false};
    // Each stored by the thread that failed and rethrown by run() after both are joined, which is why
    // they need no atomic of their own.
    std::exception_ptr producer_error_;
    std::exception_ptr consumer_error_;
    LatencySummary summary_;
    LatencyHistogram queue_latency_;
    LatencyHistogram processing_latency_;
    LatencyHistogram total_latency_;
};
