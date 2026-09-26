#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "market/market_event.h"
#include "orderbook/l2_market_view.h"
#include "orderbook/orderbook.h"

class L2OrderBook
{
   public:
    void apply_snapshot(const MarketDepthSnapshot& snapshot);
    void apply_incremental_batch(const IncrementalBookBatch& batch);
    TopOfBook top_of_book() const;
    L2MarketView market_view(std::size_t levels = 5) const;
    DepthLevel level(Side side, std::size_t index) const;
    std::size_t depth(Side side) const;
    uint64_t sequence() const;
    const std::string& symbol() const
    {
        return symbol_;
    }

   private:
    using BidLevels = std::map<double, uint64_t, std::greater<double>>;
    using AskLevels = std::map<double, uint64_t>;

    mutable std::mutex mutex_;
    std::string symbol_;
    BidLevels bids_;
    AskLevels asks_;
    uint64_t timestamp_{};
    uint64_t local_timestamp_{};
    uint64_t sequence_{};
    bool snapshot_batch_active_{false};
};