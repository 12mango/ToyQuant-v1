#include "market/threaded_l2_replay.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace
{
int64_t monotonic_now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

std::string LatencySummary::to_log_string() const
{
    // Microseconds, like the pacing line, because that is the unit these numbers are argued in and
    // the bucketed percentiles carry no precision finer than that anyway.
    const auto us = [](int64_t ns) { return ns / 1000; };
    return "handoffs=" + std::to_string(handoffs) +
           " queue_mean_us=" + std::to_string(us(queue_mean_ns)) +
           " queue_p50_us=" + std::to_string(us(queue_p50_ns)) +
           " queue_p99_us=" + std::to_string(us(queue_p99_ns)) +
           " queue_max_us=" + std::to_string(us(queue_max_ns)) +
           " processing_mean_us=" + std::to_string(us(processing_mean_ns)) +
           " processing_p50_us=" + std::to_string(us(processing_p50_ns)) +
           " processing_p99_us=" + std::to_string(us(processing_p99_ns)) +
           " processing_max_us=" + std::to_string(us(processing_max_ns)) +
           " total_mean_us=" + std::to_string(us(total_mean_ns)) +
           " total_p50_us=" + std::to_string(us(total_p50_ns)) +
           " total_p99_us=" + std::to_string(us(total_p99_ns)) +
           " total_max_us=" + std::to_string(us(total_max_ns)) +
           " ring_depth_max=" + std::to_string(ring_depth_max) +
           " feed_waits=" + std::to_string(feed_waits) +
           " feed_wait_total_us=" + std::to_string(us(feed_wait_total_ns)) +
           " feed_wait_max_us=" + std::to_string(us(feed_wait_max_ns));
}

ThreadedReplayFeed::ThreadedReplayFeed(std::unique_ptr<IMarketEventReader> trades,
                                       std::unique_ptr<IMarketEventReader> depth,
                                       EventCallback consumer, ReplayPacing pacing,
                                       MarketDataValidationConfig validation_config,
                                       std::size_t ring_capacity)
    : consumer_(std::move(consumer)),
      ring_(std::make_unique<concurrency::SpscRing<StampedEvent>>(ring_capacity))
{
    if (!consumer_) throw std::invalid_argument("a threaded replay needs a consumer callback");
    // The feed is handed a producer callback rather than the consumer one, so every event it delivers
    // is stamped and parked on the ring instead of being processed on the feed thread.
    feed_ = std::make_unique<L2ReplayFeed>(std::move(trades), std::move(depth),
                                           [this](const MarketEvent& event) { produce(event); }, pacing,
                                           validation_config);
}

void ThreadedReplayFeed::set_profiler(StageProfiler* profiler)
{
    feed_->set_profiler(profiler);
}

void ThreadedReplayFeed::run()
{
    std::thread producer([this] { producer_loop(); });
    consume_loop();
    // The consumer is the calling thread, so its own toil is done before the join; the join is what
    // makes the producer's writes to the summary visible here without another atomic.
    producer.join();

    summary_.handoffs = total_latency_.count();
    summary_.queue_mean_ns = queue_latency_.mean_ns();
    summary_.queue_p50_ns = queue_latency_.percentile_ns(0.50);
    summary_.queue_p99_ns = queue_latency_.percentile_ns(0.99);
    summary_.queue_max_ns = queue_latency_.max_ns();
    summary_.processing_mean_ns = processing_latency_.mean_ns();
    summary_.processing_p50_ns = processing_latency_.percentile_ns(0.50);
    summary_.processing_p99_ns = processing_latency_.percentile_ns(0.99);
    summary_.processing_max_ns = processing_latency_.max_ns();
    summary_.total_mean_ns = total_latency_.mean_ns();
    summary_.total_p50_ns = total_latency_.percentile_ns(0.50);
    summary_.total_p99_ns = total_latency_.percentile_ns(0.99);
    summary_.total_max_ns = total_latency_.max_ns();

    // Whichever side failed, both threads are joined by now and the failure is rethrown on the
    // caller's thread. The consumer's is preferred because it is the one a caller can act on.
    if (consumer_error_) std::rethrow_exception(consumer_error_);
    if (producer_error_) std::rethrow_exception(producer_error_);
}

void ThreadedReplayFeed::producer_loop()
{
    try
    {
        feed_->run();
    }
    catch (...)
    {
        producer_error_ = std::current_exception();
        // The consumer has to be told, or it would spin waiting for events that are never coming while
        // the failure waits in a variable nobody reads.
        abort_.store(true, std::memory_order_release);
    }
    // Published last, and with release, so a consumer that sees it also sees every event that was
    // pushed before it.
    producer_done_.store(true, std::memory_order_release);
}

void ThreadedReplayFeed::produce(const MarketEvent& event)
{
    StampedEvent stamped;
    stamped.feed_ns = monotonic_now_ns();
    // A copy is unavoidable here: a reader's event points into its own row buffer, so the value has to
    // be owned before the reader reads on. The event shapes keep that cheap in the common case — the
    // symbols are short enough for the small-string buffer and a batch keeps its first four level
    // changes inline — so a typical hand-off copies without allocating.
    stamped.event = event;

    if (ring_->try_push(std::move(stamped))) return;

    // A full ring is backpressure rather than a drop: a replay that lost an event would not be the run
    // anybody asked for, so the feed waits for room. The wait is charged to the feed's lateness against
    // its own schedule, which is where a strategy that cannot keep up is meant to show up.
    const int64_t wait_start_ns = monotonic_now_ns();
    while (!ring_->try_push(std::move(stamped)))
    {
        if (abort_.load(std::memory_order_acquire))
            throw std::runtime_error("the consumer stopped while the feed waited for room on the ring");
        concurrency::spin_relax();
    }
    const int64_t wait_ns = monotonic_now_ns() - wait_start_ns;
    ++summary_.feed_waits;
    summary_.feed_wait_total_ns += wait_ns;
    if (wait_ns > summary_.feed_wait_max_ns) summary_.feed_wait_max_ns = wait_ns;
}

void ThreadedReplayFeed::consume_loop()
{
    StampedEvent stamped;
    while (true)
    {
        if (!ring_->try_pop(stamped))
        {
            if (abort_.load(std::memory_order_acquire)) break;
            if (!producer_done_.load(std::memory_order_acquire))
            {
                // No sleep and no condition variable: at the rates this path runs at, waking a thread
                // costs more than the wait it replaces, and a consumer that slept through an arrival
                // would be the one thing that makes the hand-off slow. The occupancy this path reports
                // is what would say otherwise.
                concurrency::spin_relax();
                continue;
            }
            // The producer is done, but its last publish may have become visible after the pop above
            // failed and before the flag was read. The acquire load of the flag pairs with the release
            // store that followed the last push, so one more attempt is guaranteed to see the event.
            if (!ring_->try_pop(stamped)) break;
        }

        const int64_t pop_ns = monotonic_now_ns();
        const uint64_t waiting = ring_->consumer_occupancy() + 1;
        if (waiting > summary_.ring_depth_max) summary_.ring_depth_max = waiting;
        try
        {
            consumer_(stamped.event);
        }
        catch (...)
        {
            // The event that failed is not counted, and the producer is told to stop waiting: both are
            // joined in run(), which rethrows this.
            consumer_error_ = std::current_exception();
            abort_.store(true, std::memory_order_release);
            break;
        }
        const int64_t done_ns = monotonic_now_ns();
        queue_latency_.add(pop_ns - stamped.feed_ns);
        processing_latency_.add(done_ns - pop_ns);
        total_latency_.add(done_ns - stamped.feed_ns);
    }
}
