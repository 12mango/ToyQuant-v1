#pragma once

#include "app/pipeline.h"
#include "exchange/matching_engine.h"
#include "legacy/tick.h"
#include "orderbook/orderbook.h"

namespace legacy
{

class TickPipeline
{
   public:
    TickPipeline(Pipeline& pipeline, IOrderBook& market_book, ILegacyTickBook& legacy_book,
                 IMatchingEngine& engine)
        : pipeline_(pipeline), market_book_(market_book), legacy_book_(legacy_book), engine_(engine)
    {
    }

    void process(const Tick& tick, bool enable_print = true)
    {
        legacy_book_.on_tick(tick);
        engine_.process_market_trade(
            MarketTrade{tick.ts, tick.symbol, tick.price, tick.size, tick.side, 0, {}});
        pipeline_.process_top_of_book(tick.symbol, tick.ts, market_book_.market_top(tick.symbol),
                          enable_print);
    }

   private:
    Pipeline& pipeline_;
    IOrderBook& market_book_;
    ILegacyTickBook& legacy_book_;
    IMatchingEngine& engine_;
};

}  // namespace legacy
