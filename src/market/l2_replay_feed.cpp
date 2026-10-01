#include "market/l2_replay_feed.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace
{
uint64_t event_timestamp(const MarketEvent& event)
{
    return std::visit(
        [](const auto& value)
        {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, IncrementalBookBatch>)
                return value.exchange_ts;
            else
                return value.ts;
        },
        event);
}

int64_t monotonic_now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void real_sleep(int64_t ns)
{
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}
}  // namespace

std::string PacingSummary::to_log_string() const
{
    // Microseconds are the unit a feed's lateness is argued in, and the buckets are powers of two microseconds,
    // so the percentiles below carry the bucket they came from rather than a precision they do not have.
    const auto us = [](int64_t ns) { return ns / 1000; };
    return "paced_events=" + std::to_string(events) + " late_events=" + std::to_string(late_events) +
           " lateness_mean_us=" + std::to_string(us(lateness_mean_ns)) +
           " lateness_p50_us=" + std::to_string(us(lateness_p50_ns)) +
           " lateness_p99_us=" + std::to_string(us(lateness_p99_ns)) +
           " lateness_max_us=" + std::to_string(us(lateness_max_ns)) +
           " market_window_s=" + std::to_string(market_window_ns / 1'000'000'000) +
           " wall_s=" + std::to_string(wall_ns / 1'000'000'000) + " speed=" + std::to_string(speed);
}

L2ReplayFeed::L2ReplayFeed(std::unique_ptr<IMarketEventReader> trades,
                           std::unique_ptr<IMarketEventReader> depth, EventCallback callback,
                           ReplayPacing pacing, MarketDataValidationConfig validation_config)
    : trades_(std::move(trades)),
      depth_(std::move(depth)),
      callback_(std::move(callback)),
      pacing_(pacing),
      validator_(validation_config),
      clock_(monotonic_now_ns),
      sleeper_(real_sleep)
{
    if (!trades_ || !depth_) throw std::invalid_argument("L2 replay requires trade and depth readers");
    if (!callback_) throw std::invalid_argument("L2 replay callback cannot be empty");
    if (pacing_.speed <= 0.0) throw std::invalid_argument("replay pacing speed must be positive");
    if (pacing_.max_lateness_ns < 0) throw std::invalid_argument("replay pacing lateness cannot be negative");
}

void L2ReplayFeed::record_lateness(int64_t lateness_ns)
{
    ++pacing_summary_.late_events;
    lateness_sum_ns_ += lateness_ns;
    ++lateness_count_;
    if (lateness_ns > pacing_summary_.lateness_max_ns) pacing_summary_.lateness_max_ns = lateness_ns;
    // Powers of two in microseconds, with the first bucket holding everything under a microsecond.
    std::size_t bucket = 0;
    int64_t step = 1;
    while (bucket + 1 < kLatenessBuckets && lateness_ns >= step * 1000)
    {
        ++bucket;
        step *= 2;
    }
    ++lateness_buckets_[bucket];
}

void L2ReplayFeed::pace(int64_t event_ns)
{
    if (!started_)
    {
        started_ = true;
        first_event_ns_ = event_ns;
        start_wall_ns_ = clock_();
    }
    const double offset = static_cast<double>(event_ns - first_event_ns_) / pacing_.speed;
    const int64_t target_ns = start_wall_ns_ + static_cast<int64_t>(offset);
    const int64_t now_ns = clock_();
    const int64_t lateness_ns = now_ns - target_ns;
    if (lateness_ns > 0) record_lateness(lateness_ns);
    // A delivery that is further behind than the tolerance is not waited for: the schedule is in the past, so
    // sleeping would add to the lateness instead of reducing it, and the run catches up by skipping the wait.
    if (lateness_ns <= pacing_.max_lateness_ns && target_ns > now_ns) sleeper_(target_ns - now_ns);
}

void L2ReplayFeed::run()
{
    MarketEvent trade;
    MarketEvent depth;
    profiler_begin(Stage::ReadParse);
    bool has_trade = trades_->next(trade);
    bool has_depth = depth_->next(depth);
    profiler_end(Stage::ReadParse);
    uint64_t trade_timestamp = has_trade ? event_timestamp(trade) : 0;
    uint64_t depth_timestamp = has_depth ? event_timestamp(depth) : 0;
    int64_t last_event_ns = 0;

    while (has_trade || has_depth)
    {
        const bool use_depth = has_depth && (!has_trade || depth_timestamp <= trade_timestamp);
        const MarketEvent& event = use_depth ? depth : trade;

        if (pacing_.enabled)
        {
            last_event_ns = static_cast<int64_t>(event_timestamp(event));
            ++pacing_summary_.events;
            pace(last_event_ns);
        }

        if (profiler_ != nullptr) profiler_->start_event();

        profiler_begin(Stage::Validate);
        validator_.validate(event);
        profiler_end(Stage::Validate);

        // The callback brackets its own regions, so it is not timed here.
        callback_(event);

        // Reading the next event is charged to the read+parse stage of this iteration. Every
        // event still contributes exactly one sample, only the attribution is shifted by one.
        profiler_begin(Stage::ReadParse);
        if (use_depth)
        {
            has_depth = depth_->next(depth);
            if (has_depth) depth_timestamp = event_timestamp(depth);
        }
        else
        {
            has_trade = trades_->next(trade);
            if (has_trade) trade_timestamp = event_timestamp(trade);
        }
        profiler_end(Stage::ReadParse);
    }

    if (pacing_.enabled)
    {
        pacing_summary_.speed = pacing_.speed;
        pacing_summary_.market_window_ns = last_event_ns - first_event_ns_;
        pacing_summary_.wall_ns = clock_() - start_wall_ns_;
        if (lateness_count_ > 0)
            pacing_summary_.lateness_mean_ns =
                lateness_sum_ns_ / static_cast<int64_t>(lateness_count_);
        // The percentile is the upper edge of the bucket the sample fell in, which is why the report says
        // microseconds rather than claiming a precision the histogram does not have.
        // The percentile is the upper edge of the bucket the sample fell in, clamped to the observed maximum:
        // without the clamp a reported p99 can exceed the largest sample, which reads as a broken metric even
        // though it is the histogram's granularity showing through.
        const auto percentile = [&](double fraction)
        {
            if (lateness_count_ == 0) return int64_t{0};
            const uint64_t target = std::max<uint64_t>(
                1, static_cast<uint64_t>(fraction * static_cast<double>(lateness_count_)));
            uint64_t seen = 0;
            for (std::size_t bucket = 0; bucket < lateness_buckets_.size(); ++bucket)
            {
                seen += lateness_buckets_[bucket];
                if (seen >= target && lateness_buckets_[bucket] > 0)
                    return std::min(
                        static_cast<int64_t>(bucket == 0 ? 1 : (int64_t{1} << bucket)) * 1000,
                        pacing_summary_.lateness_max_ns);
            }
            return pacing_summary_.lateness_max_ns;
        };
        pacing_summary_.lateness_p50_ns = percentile(0.50);
        pacing_summary_.lateness_p99_ns = percentile(0.99);
    }

    if (validator_.summary().events == 0)
        throw std::invalid_argument("L2 replay input contains no market events");
}
