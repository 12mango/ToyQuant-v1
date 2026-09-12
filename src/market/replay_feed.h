#pragma once

#include <functional>

#include "market/market_data_adapter.h"

class ReplayFeed
{
   public:
    using EventCallback = std::function<void(const MarketEvent&)>;

    ReplayFeed(MarketDataReaders readers, EventCallback callback, int ms_delay = 0);
    void run();

   private:
    MarketDataReaders readers_;
    EventCallback callback_;
    int ms_delay_;
};
