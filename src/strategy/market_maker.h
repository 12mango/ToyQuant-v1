#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <iostream>
#include <numeric>
#include <unordered_map>

#include "exchange/execution_report.h"
#include "orderbook/orderbook.h"
#include "strategy.h"

class NaiveMarketMaker : public Strategy
{
   public:
    uint64_t base_order_size;
    double base_spread;
    double tick_size;
    std::unordered_map<uint64_t, StrategyOrder> open_orders;
    int64_t position{0};

    NaiveMarketMaker(uint64_t size = 50, double spd = 0.00003, double ts = 0.00001)
        : base_order_size(size), base_spread(spd), tick_size(ts)
    {
    }

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& tob) override
    {
        std::vector<StrategyOrder> orders;

        if (tob.bid_price <= 0 || tob.ask_price <= 0) return orders;

        double mid = (tob.bid_price + tob.ask_price) / 2.0;
        double buy_price = std::round((mid - base_spread / 2.0) / tick_size) * tick_size;
        double sell_price = std::round((mid + base_spread / 2.0) / tick_size) * tick_size;

        StrategyOrder buy_order(Side::Buy, symbol, buy_price, base_order_size, 0);
        StrategyOrder sell_order(Side::Sell, symbol, sell_price, base_order_size, 0);

        orders.push_back(buy_order);
        orders.push_back(sell_order);
        return orders;
    }

    void on_order_submitted(const StrategyOrder& order) override
    {
        open_orders[order.order_id] = order;
    }

    std::vector<uint64_t> cancel_requests() override
    {
        return {};
    }

    int64_t net_position() const override
    {
        return position;
    }
    size_t working_order_count() const override
    {
        return open_orders.size();
    }

    void on_order_update(const ExecutionReport& report) override
    {
        if (report.exec_type == ExecType::Cancelled)
        {
            open_orders.erase(report.order_id);
            return;
        }

        if (report.exec_type == ExecType::Filled)
        {
            open_orders.erase(report.order_id);
            return;
        }

        if (report.exec_type == ExecType::Resting) return;

        if (report.exec_type == ExecType::PartialFill) return;

        if (report.exec_type == ExecType::Trade)
        {
            auto it = open_orders.find(report.order_id);
            if (it != open_orders.end())
            {
                position += report.side == exchange::Side::Buy
                                ? static_cast<int64_t>(report.quantity)
                                : -static_cast<int64_t>(report.quantity);
                it->second.quantity =
                    (it->second.quantity > report.quantity ? it->second.quantity - report.quantity
                                                           : 0);
                if (it->second.quantity == 0) open_orders.erase(it);
            }
        }
    }
};

class OptimizedMarketMaker : public Strategy
{
   public:
    uint64_t base_order_size;
    double base_spread;
    int64_t inventory_limit;
    double tick_size;
    int smooth_N;
    std::unordered_map<uint64_t, StrategyOrder> open_orders;
    std::deque<double> mid_prices;
    double last_mid;
    double max_level_multiplier;
    int64_t position;
    double last_quote_mid;
    uint64_t quote_age_ticks;
    uint64_t max_quote_age_ticks;
    uint64_t quote_refresh_ticks;

    OptimizedMarketMaker(uint64_t size = 50, double spd = 0.00003, int64_t inv_limit = 1000,
                         double ts = 0.00001, int smooth_window = 10, double level_multiplier = 3.0,
                         uint64_t max_quote_age = 20, uint64_t refresh_ticks = 5)
        : base_order_size(size),
          base_spread(spd),
          inventory_limit(inv_limit),
          tick_size(ts),
          smooth_N(smooth_window),
          last_mid(0.0),
          max_level_multiplier(level_multiplier),
          position(0),
          last_quote_mid(0.0),
          quote_age_ticks(0),
          max_quote_age_ticks(max_quote_age),
          quote_refresh_ticks(refresh_ticks)
    {
    }

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& tob) override
    {
        std::vector<StrategyOrder> orders;

        if (tob.bid_price <= 0 || tob.ask_price <= 0) return orders;

        double mid = (tob.bid_price + tob.ask_price) / 2.0;
        if (mid_prices.empty())
        {
            last_mid = mid;
            mid_prices.push_back(mid);
        }
        else
        {
            mid_prices.push_back(mid);
            if ((int)mid_prices.size() > smooth_N) mid_prices.pop_front();
        }

        double smooth_mid =
            std::accumulate(mid_prices.begin(), mid_prices.end(), 0.0) / mid_prices.size();

        double trend = 0.0;
        if (mid_prices.size() > 1) trend = smooth_mid - last_mid;
        last_mid = smooth_mid;

        if (!open_orders.empty()) ++quote_age_ticks;
        if (!open_orders.empty() && quote_age_ticks < max_quote_age_ticks &&
            std::abs(smooth_mid - last_quote_mid) < quote_refresh_ticks * tick_size)
        {
            return orders;
        }

        double tick_vol = std::abs(tob.ask_price - tob.bid_price);
        double spread_adj = base_spread;
        if (tick_vol > base_spread * 5) spread_adj = base_spread * 2;
        if (tick_vol > base_spread * 20) spread_adj = base_spread * 3;

        int64_t working_exposure = 0;
        for (const auto& entry : open_orders)
        {
            const StrategyOrder& order = entry.second;
            working_exposure += order.side == Side::Buy ? static_cast<int64_t>(order.quantity)
                                                        : -static_cast<int64_t>(order.quantity);
        }
        const int64_t inventory = position + working_exposure;

        double inv_frac = 0.0;
        if (inventory_limit > 0)
            inv_frac = std::min(1.0, std::abs(double(inventory)) / double(inventory_limit));
        double inv_spread_bias = inv_frac * base_spread;

        uint64_t level_count = 3;
        for (uint64_t level = 1; level <= level_count; ++level)
        {
            double level_mul = std::min(max_level_multiplier, double(level));
            double level_spread = spread_adj * level_mul;

            double raw_buy = smooth_mid - level_spread / 2.0 - std::max(0.0, trend) -
                             inv_spread_bias * (inventory > 0 ? 1.0 : 0.0);
            double raw_sell = smooth_mid + level_spread / 2.0 + std::max(0.0, trend) +
                              inv_spread_bias * (inventory < 0 ? 1.0 : 0.0);

            double buy_price = std::round(raw_buy / tick_size) * tick_size;
            double sell_price = std::round(raw_sell / tick_size) * tick_size;

            double buy_qty_scale = 1.0;
            double sell_qty_scale = 1.0;
            if (inventory > 0)
            {
                buy_qty_scale = std::max(0.0, 1.0 - double(inventory) / double(inventory_limit));
            }
            if (inventory < 0)
            {
                sell_qty_scale = std::max(0.0, 1.0 - double(-inventory) / double(inventory_limit));
            }

            uint64_t buy_qty = uint64_t(
                std::max(0.0, std::floor(base_order_size * buy_qty_scale / double(level))));
            uint64_t sell_qty = uint64_t(
                std::max(0.0, std::floor(base_order_size * sell_qty_scale / double(level))));

            if (inventory > inventory_limit) buy_qty = 0;
            if (inventory < -inventory_limit) sell_qty = 0;

            if (buy_qty > 0)
            {
                StrategyOrder buy_order(Side::Buy, symbol, buy_price, buy_qty, 0);
                orders.push_back(buy_order);
            }

            if (sell_qty > 0)
            {
                StrategyOrder sell_order(Side::Sell, symbol, sell_price, sell_qty, 0);
                orders.push_back(sell_order);
            }
        }

        return orders;
    }

    void on_order_submitted(const StrategyOrder& order) override
    {
        open_orders[order.order_id] = order;
        last_quote_mid = last_mid;
        quote_age_ticks = 0;
    }

    std::vector<uint64_t> cancel_requests() override
    {
        if (last_quote_mid == 0.0 ||
            (quote_age_ticks < max_quote_age_ticks &&
             std::abs(last_mid - last_quote_mid) < quote_refresh_ticks * tick_size))
        {
            return {};
        }

        std::vector<uint64_t> order_ids;
        order_ids.reserve(open_orders.size());
        for (const auto& entry : open_orders)
        {
            order_ids.push_back(entry.first);
        }
        return order_ids;
    }

    int64_t net_position() const override
    {
        return position;
    }
    size_t working_order_count() const override
    {
        return open_orders.size();
    }

    void on_order_update(const ExecutionReport& report) override
    {
        if (report.exec_type == ExecType::Cancelled)
        {
            open_orders.erase(report.order_id);
            return;
        }

        if (report.exec_type == ExecType::Filled)
        {
            open_orders.erase(report.order_id);
            return;
        }

        if (report.exec_type == ExecType::Resting) return;

        if (report.exec_type == ExecType::PartialFill) return;

        if (report.exec_type == ExecType::Trade)
        {
            auto it = open_orders.find(report.order_id);
            if (it != open_orders.end())
            {
                position += report.side == exchange::Side::Buy
                                ? static_cast<int64_t>(report.quantity)
                                : -static_cast<int64_t>(report.quantity);
                it->second.quantity =
                    (it->second.quantity > report.quantity ? it->second.quantity - report.quantity
                                                           : 0);
                if (it->second.quantity == 0) open_orders.erase(it);
            }
        }
    }
};
