#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <map>
#include <memory_resource>
#include <mutex>
#include <string>
#include <vector>

#include "common/types.h"
#include "market/market_event.h"
#include "orderbook/l2_market_view.h"
#include "orderbook/orderbook.h"

class L2OrderBook
{
   public:
    explicit L2OrderBook(double tick_size = PRICE_TICK_SIZE)
                : tick_size_(tick_size),
                    price_scale_(1.0 / tick_size),
                    bids_(&level_memory_),
                    asks_(&level_memory_)
    {
    }

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
    using BidLevels = std::pmr::map<PriceTick, uint64_t, std::greater<PriceTick>>;
    using AskLevels = std::pmr::map<PriceTick, uint64_t>;

    double price_from_tick(PriceTick tick) const
    {
        const double price = to_price(tick, tick_size_);
        return price_scale_ > 0.0 ? std::round(price * price_scale_) / price_scale_ : price;
    }

    mutable std::mutex mutex_;
    std::string symbol_;
    double tick_size_;
    double price_scale_;
    std::pmr::unsynchronized_pool_resource level_memory_;
    BidLevels bids_;
    AskLevels asks_;
    uint64_t timestamp_{};
    uint64_t local_timestamp_{};
    uint64_t sequence_{};
    bool snapshot_batch_active_{false};
};