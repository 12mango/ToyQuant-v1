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

class InventoryAwareL2MarketMaker final : public Strategy
{
   public:
    explicit InventoryAwareL2MarketMaker(InventoryAwareL2MarketMakerConfig config)
        : config_(config), base_(config.base)
    {
    }

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& top) override
    {
        return adjust_orders(base_.on_top_of_book(symbol, top));
    }

    std::vector<StrategyOrder> on_l2_market_view(const L2MarketView& view) override
    {
        return adjust_orders(base_.on_l2_market_view(view));
    }

    void on_market_trade(const MarketTrade& trade) override
    {
        base_.on_market_trade(trade);
    }

    void on_order_submitted(const StrategyOrder& order) override
    {
        base_.on_order_submitted(order);
    }

    std::vector<uint64_t> cancel_requests() override
    {
        return base_.cancel_requests();
    }

    int64_t net_position() const override
    {
        return base_.net_position();
    }

    std::size_t working_order_count() const override
    {
        return base_.working_order_count();
    }

    StrategyMetrics metrics() const override
    {
        return base_.metrics();
    }

    void on_order_update(const ExecutionReport& report) override
    {
        base_.on_order_update(report);
    }

   private:
    std::vector<StrategyOrder> adjust_orders(std::vector<StrategyOrder> orders) const
    {
        const int64_t position = base_.net_position();
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
    L2MarketMaker base_;
};
