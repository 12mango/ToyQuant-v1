#include "market/l2_replay_feed.h"

#include <chrono>
#include <stdexcept>
#include <thread>

namespace
{
uint64_t event_timestamp(const MarketEvent& event)
{
    return std::visit([](const auto& value) { return value.ts; }, event);
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
    bool has_trade = trades_->next(trade);
    bool has_depth = depth_->next(depth);

    while (has_trade || has_depth)
    {
        const bool use_depth =
            has_depth && (!has_trade || event_timestamp(depth) <= event_timestamp(trade));
        const MarketEvent& event = use_depth ? depth : trade;
        validator_.validate(event);
        callback_(event);
        if (use_depth)
            has_depth = depth_->next(depth);
        else
            has_trade = trades_->next(trade);
        if (ms_delay_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms_delay_));
    }

    if (validator_.summary().events == 0)
        throw std::invalid_argument("L2 replay input contains no market events");
}
