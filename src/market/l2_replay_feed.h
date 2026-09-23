#pragma once

#include <functional>
#include <memory>

#include "market/market_data_adapter.h"
#include "market/market_data_validator.h"

class L2ReplayFeed
{
   public:
    using EventCallback = std::function<void(const MarketEvent&)>;

    L2ReplayFeed(std::unique_ptr<IMarketEventReader> trades,
                 std::unique_ptr<IMarketEventReader> depth,
                 EventCallback callback, int ms_delay = 0,
                 MarketDataValidationConfig validation_config = {});

    void run();
    const MarketDataValidationSummary& validation_summary() const
    {
        return validator_.summary();
    }

   private:
    std::unique_ptr<IMarketEventReader> trades_;
    std::unique_ptr<IMarketEventReader> depth_;
    EventCallback callback_;
    int ms_delay_;
    MarketDataValidator validator_;
};
