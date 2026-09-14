#include "market/replay_feed.h"

#include <chrono>
#include <thread>

namespace
{
uint64_t event_timestamp(const MarketEvent& event)
{
    return std::visit([](const auto& value) { return value.ts; }, event);
}
}  // namespace

ReplayFeed::ReplayFeed(MarketDataReaders readers, EventCallback callback, int ms_delay,
                       MarketDataValidationConfig validation_config)
    : readers_(std::move(readers)),
      callback_(std::move(callback)),
      ms_delay_(ms_delay),
      validator_(validation_config)
{
}

void ReplayFeed::run()
{
    MarketEvent trade;
    MarketEvent quote;
    bool has_trade = readers_.trades->next(trade);
    bool has_quote = readers_.quotes->next(quote);

    while (has_trade || has_quote)
    {
        const bool use_quote =
            has_quote && (!has_trade || event_timestamp(quote) <= event_timestamp(trade));
        const MarketEvent& event = use_quote ? quote : trade;
        validator_.validate(event);
        callback_(event);
        if (use_quote)
            has_quote = readers_.quotes->next(quote);
        else
            has_trade = readers_.trades->next(trade);

        if (ms_delay_ > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms_delay_));
    }
}
