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

class L1MarketMaker : public Strategy
{
   public:
    uint64_t base_order_size;
    double base_spread;
    int64_t inventory_limit;
    double tick_size;
    std::unordered_map<uint64_t, StrategyOrder> open_orders;
    int64_t position{0};
    double last_quote_mid{0.0};
    double last_bid_price{0.0};
    double last_ask_price{0.0};
    bool cancel_pending{false};
    struct TradeSample
    {
        Side side;
        uint64_t quantity;
    };
    std::deque<TradeSample> recent_trades;
    uint64_t buy_volume{0};
    uint64_t sell_volume{0};
    std::size_t trade_imbalance_window{32};
    std::deque<double> recent_mids;
    std::deque<double> recent_mid_changes;
    std::size_t volatility_window{16};
    uint64_t quote_age{0};
    uint64_t max_quote_age{20};
    double max_book_spread_ratio{0.02};
    double inventory_risk_threshold{0.75};
    double severe_spread_multiplier{20.0};

    L1MarketMaker(uint64_t size = 50, double spd = 0.00003, int64_t inv_limit = 1000,
                  double ts = 0.00001)
        : base_order_size(size), base_spread(spd), inventory_limit(inv_limit), tick_size(ts)
    {
    }

    void on_market_trade(const MarketTrade& trade) override
    {
        if (trade.aggressor_side != Side::Buy && trade.aggressor_side != Side::Sell) return;

        recent_trades.push_back({trade.aggressor_side, trade.quantity});
        if (trade.aggressor_side == Side::Buy)
            buy_volume += trade.quantity;
        else
            sell_volume += trade.quantity;

        while (recent_trades.size() > trade_imbalance_window)
        {
            const auto stale_trade = recent_trades.front();
            recent_trades.pop_front();

            if (stale_trade.side == Side::Buy)
            {
                if (buy_volume >= stale_trade.quantity)
                    buy_volume -= stale_trade.quantity;
                else
                    buy_volume = 0;
            }
            else
            {
                if (sell_volume >= stale_trade.quantity)
                    sell_volume -= stale_trade.quantity;
                else
                    sell_volume = 0;
            }
        }
    }

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& tob) override
    {
        std::vector<StrategyOrder> orders;
        if (tob.bid_price <= 0.0 || tob.ask_price <= 0.0 || tob.bid_price > tob.ask_price)
        {
            cancel_pending = !open_orders.empty();
            return orders;
        }

        const double mid = (tob.bid_price + tob.ask_price) / 2.0;
        const double market_spread = tob.ask_price - tob.bid_price;
        const double spread_ratio = market_spread / std::max(mid, 1.0);
        const double severe_spread_threshold =
            std::max(mid * max_book_spread_ratio * severe_spread_multiplier,
                     base_spread * severe_spread_multiplier);
        if (market_spread <= 0.0 || spread_ratio > max_book_spread_ratio ||
            market_spread > severe_spread_threshold)
        {
            cancel_pending = !open_orders.empty();
            return orders;
        }

        update_mid_history(mid);
        if (!open_orders.empty()) ++quote_age;

        const int64_t working_exposure = working_position();
        const int64_t inventory = position + working_exposure;
        const double inventory_ratio =
            inventory_limit > 0
                ? std::clamp(static_cast<double>(inventory) / static_cast<double>(inventory_limit),
                             -1.0, 1.0)
                : 0.0;
        const double inventory_skew = std::tanh(inventory_ratio) * base_spread;
        const double reservation_mid = mid - inventory_skew;
        const double imbalance = trade_imbalance();
        const double book_imbalance = tob_imbalance(tob);
        const double adverse_shift = (std::abs(imbalance) * base_spread * 0.5) +
                                     (std::abs(book_imbalance) * base_spread * 0.25);
        const double effective_spread = dynamic_spread(tob);

        const double raw_bid = reservation_mid - effective_spread / 2.0 - adverse_shift;
        const double raw_ask = reservation_mid + effective_spread / 2.0 + adverse_shift;
        const double bid_price = std::min(tob.bid_price, round_price(raw_bid));
        const double ask_price = std::max(tob.ask_price, round_price(raw_ask));
        if (bid_price >= ask_price)
        {
            cancel_pending = !open_orders.empty();
            return orders;
        }

        uint64_t buy_quantity = base_order_size;
        uint64_t sell_quantity = base_order_size;
        if (inventory > 0)
            buy_quantity = scaled_quantity(inventory_limit - std::min(inventory, inventory_limit));
        if (inventory < 0)
            sell_quantity =
                scaled_quantity(inventory_limit - std::min(-inventory, inventory_limit));

        const double risk_threshold =
            inventory_limit > 0 ? static_cast<double>(inventory_limit) * inventory_risk_threshold
                                : 0.0;
        if (inventory > risk_threshold)
        {
            buy_quantity = 0;
            sell_quantity = std::max<uint64_t>(sell_quantity, 1);
        }
        else if (inventory < -risk_threshold)
        {
            sell_quantity = 0;
            buy_quantity = std::max<uint64_t>(buy_quantity, 1);
        }

        if (should_refresh(mid, bid_price, ask_price))
        {
            if (!open_orders.empty())
            {
                cancel_pending = true;
                return orders;
            }
            if (buy_quantity > 0)
                orders.emplace_back(Side::Buy, symbol, bid_price, buy_quantity, 0);
            if (sell_quantity > 0)
                orders.emplace_back(Side::Sell, symbol, ask_price, sell_quantity, 0);
            last_quote_mid = mid;
            last_bid_price = bid_price;
            last_ask_price = ask_price;
        }
        return orders;
    }

    void on_order_submitted(const StrategyOrder& order) override
    {
        open_orders[order.order_id] = order;
        quote_age = 0;
    }

    std::vector<uint64_t> cancel_requests() override
    {
        if (!cancel_pending) return {};

        std::vector<uint64_t> order_ids;
        order_ids.reserve(open_orders.size());
        for (const auto& entry : open_orders) order_ids.push_back(entry.first);
        cancel_pending = false;
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
        if (report.exec_type == ExecType::Trade)
        {
            auto it = open_orders.find(report.order_id);
            if (it != open_orders.end())
            {
                position += report.side == exchange::Side::Buy
                                ? static_cast<int64_t>(report.quantity)
                                : -static_cast<int64_t>(report.quantity);
                it->second.quantity = it->second.quantity > report.quantity
                                          ? it->second.quantity - report.quantity
                                          : 0;
                if (it->second.quantity == 0) open_orders.erase(it);
            }
        }
        else if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
        {
            open_orders.erase(report.order_id);
        }
    }

   private:
    int64_t working_position() const
    {
        int64_t exposure = 0;
        for (const auto& entry : open_orders)
        {
            exposure += entry.second.side == Side::Buy
                            ? static_cast<int64_t>(entry.second.quantity)
                            : -static_cast<int64_t>(entry.second.quantity);
        }
        return exposure;
    }

    void update_mid_history(double mid)
    {
        if (!recent_mids.empty())
        {
            recent_mid_changes.push_back(std::abs(mid - recent_mids.back()));
            if (recent_mid_changes.size() > volatility_window) recent_mid_changes.pop_front();
        }
        recent_mids.push_back(mid);
        if (recent_mids.size() > volatility_window + 1) recent_mids.pop_front();
    }

    double dynamic_spread(const TopOfBook& tob) const
    {
        const double market_spread = tob.ask_price - tob.bid_price;
        double average_mid_change = 0.0;
        for (double change : recent_mid_changes) average_mid_change += change;
        if (!recent_mid_changes.empty()) average_mid_change /= recent_mid_changes.size();
        return std::max(base_spread, market_spread) + 2.0 * average_mid_change;
    }

    double tob_imbalance(const TopOfBook& tob) const
    {
        const uint64_t total_size = tob.bid_size + tob.ask_size;
        if (total_size == 0) return 0.0;
        return (static_cast<double>(tob.bid_size) - static_cast<double>(tob.ask_size)) /
               static_cast<double>(total_size);
    }

    uint64_t scaled_quantity(int64_t available_inventory) const
    {
        if (inventory_limit <= 0 || available_inventory <= 0) return 0;
        const double scale =
            static_cast<double>(available_inventory) / static_cast<double>(inventory_limit);
        return static_cast<uint64_t>(std::floor(base_order_size * scale));
    }

    double round_price(double price) const
    {
        return std::round(price / tick_size) * tick_size;
    }

    double trade_imbalance() const
    {
        const uint64_t total_volume = buy_volume + sell_volume;
        if (total_volume == 0) return 0.0;
        return (static_cast<double>(buy_volume) - static_cast<double>(sell_volume)) /
               static_cast<double>(total_volume);
    }

    bool should_refresh(double mid, double bid_price, double ask_price) const
    {
        if (open_orders.empty()) return true;
        if (last_quote_mid == 0.0) return true;

        const bool minor_movement = std::abs(mid - last_quote_mid) < 2.0 * tick_size &&
                                    std::abs(bid_price - last_bid_price) < tick_size &&
                                    std::abs(ask_price - last_ask_price) < tick_size;
        if (minor_movement && quote_age < max_quote_age) return false;

        return std::abs(mid - last_quote_mid) >= 2.0 * tick_size || bid_price != last_bid_price ||
               ask_price != last_ask_price || quote_age >= max_quote_age;
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
