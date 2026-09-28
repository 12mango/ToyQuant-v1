#include "market/l2_replay_feed.h"

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
}  // namespace

L2ReplayFeed::L2ReplayFeed(std::unique_ptr<IMarketEventReader> trades,
                           std::unique_ptr<IMarketEventReader> depth, EventCallback callback,
                           int ms_delay, MarketDataValidationConfig validation_config)
    : trades_(std::move(trades)),
      depth_(std::move(depth)),
      callback_(std::move(callback)),
      ms_delay_(ms_delay),
      validator_(validation_config)
{
    if (!trades_ || !depth_) throw std::invalid_argument("L2 replay requires trade and depth readers");
    if (!callback_) throw std::invalid_argument("L2 replay callback cannot be empty");
    if (ms_delay_ < 0) throw std::invalid_argument("L2 replay delay cannot be negative");
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

    while (has_trade || has_depth)
    {
        const bool use_depth = has_depth && (!has_trade || depth_timestamp <= trade_timestamp);
        const MarketEvent& event = use_depth ? depth : trade;

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

        if (ms_delay_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms_delay_));
    }

    if (validator_.summary().events == 0)
        throw std::invalid_argument("L2 replay input contains no market events");
}
