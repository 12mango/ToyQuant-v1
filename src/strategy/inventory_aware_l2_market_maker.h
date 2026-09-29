#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "strategy/l2_market_maker.h"

struct InventoryAwareL2MarketMakerConfig
{
    L2MarketMakerConfig base;
    int64_t inventory_limit{100};
    double max_inventory_shift_ticks{2.0};
};

// Adds a continuous inventory response to the L2 maker: the side that would grow the position is
// scaled down and the side that would reduce it is priced more aggressively.
//
// It derives from L2MarketMaker instead of holding one and forwarding each callback. The wrapper
// version forgot to forward on_queue_activity, which silently disabled the base's queue-hold refresh
// policy together with its queue-consumption counters: this strategy reported zero consumed queue
// while the base's own bookkeeping, and the refresh decision built on it, never ran.
class InventoryAwareL2MarketMaker final : public L2MarketMaker
{
   public:
    explicit InventoryAwareL2MarketMaker(InventoryAwareL2MarketMakerConfig config)
        : L2MarketMaker(config.base), config_(config)
    {
    }

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& top) override
    {
        return adjust_orders(L2MarketMaker::on_top_of_book(symbol, top));
    }

    std::vector<StrategyOrder> on_l2_market_view(const L2MarketView& view) override
    {
        return adjust_orders(L2MarketMaker::on_l2_market_view(view));
    }

   private:
    std::vector<StrategyOrder> adjust_orders(std::vector<StrategyOrder> orders) const
    {
        const int64_t position = net_position();
        const double ratio = config_.inventory_limit > 0
                                 ? std::clamp(std::abs(static_cast<double>(position)) /
                                                  static_cast<double>(config_.inventory_limit),
                                              0.0, 1.0)
                                 : 0.0;
        const double price_shift = ratio * config_.max_inventory_shift_ticks * config_.base.tick_size;
        for (auto& order : orders)
        {
            const bool reduces_inventory =
                (position > 0 && order.side == Side::Sell) ||
                (position < 0 && order.side == Side::Buy);
            const bool adds_inventory =
                (position > 0 && order.side == Side::Buy) ||
                (position < 0 && order.side == Side::Sell);
            if (adds_inventory)
            {
                order.quantity = static_cast<uint64_t>(std::floor(
                    static_cast<double>(order.quantity) * (1.0 - ratio)));
            }
            if (reduces_inventory)
            {
                if (position > 0 && order.side == Side::Sell) order.price -= price_shift;
                if (position < 0 && order.side == Side::Buy) order.price += price_shift;
            }
        }
        orders.erase(std::remove_if(orders.begin(), orders.end(),
                                    [](const StrategyOrder& order) { return order.quantity == 0; }),
                     orders.end());
        return orders;
    }

    InventoryAwareL2MarketMakerConfig config_;
};
