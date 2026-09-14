#pragma once

#include <functional>

#include "market/market_data_adapter.h"
#include "market/market_data_validator.h"

class ReplayFeed
{
   public:
    using EventCallback = std::function<void(const MarketEvent&)>;

    ReplayFeed(MarketDataReaders readers, EventCallback callback, int ms_delay = 0,
               MarketDataValidationConfig validation_config = {});
    void run();
    const MarketDataValidationSummary& validation_summary() const
    {
        return validator_.summary();
    }

   private:
    MarketDataReaders readers_;
    EventCallback callback_;
    int ms_delay_;
    MarketDataValidator validator_;
};
