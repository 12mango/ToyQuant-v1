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

struct L1MarketMakerConfig
{
    uint64_t order_size{50};
    double base_spread{0.00003};
    int64_t inventory_limit{1000};
    double tick_size{0.00001};
    std::size_t trade_imbalance_window{32};
    std::size_t volatility_window{16};
    uint64_t max_quote_age{20};
    double max_book_spread_ratio{0.02};
    double inventory_risk_threshold{0.60};
    double severe_spread_multiplier{20.0};
    double stress_spread_multiplier{2.5};
    double minimum_stress_quantity_ratio{0.10};
    double maker_fee_rate{0.0002};
    double fee_spread_multiplier{0.40};
    uint64_t max_market_trade_age{1000};
    double max_trade_deviation_bps{50.0};
    uint64_t markout_horizon_quotes{5};
    double flow_trade_weight{0.6};
    double flow_book_weight{0.4};
    double flow_price_threshold{0.06};
    double flow_quantity_threshold{0.08};
    double flow_max_quantity_reduction{0.20};
    uint64_t flow_price_ticks{1};
};

struct FlowAwareMarketMakerConfig
{
    double trade_weight{0.6};
    double book_weight{0.4};
    double price_threshold{0.06};
    double quantity_threshold{0.08};
    double max_quantity_reduction{0.20};
    uint64_t price_ticks{1};
};

class PassiveL1MarketMaker : public Strategy
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
    uint64_t quote_age{0};
    uint64_t max_quote_age{20};
    uint64_t quote_cycle{0};

    explicit PassiveL1MarketMaker(uint64_t size = 50, double spd = 0.00003,
                                  int64_t inv_limit = 1000, double ts = 0.00001,
                                  uint64_t max_age = 20)
        : base_order_size(size),
          base_spread(spd),
          inventory_limit(inv_limit),
          tick_size(ts),
          max_quote_age(max_age)
    {
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
        ++quote_cycle;
        if (!open_orders.empty()) ++quote_age;

        const bool should_refresh =
            open_orders.empty() ||
            std::abs(mid - last_quote_mid) >= 2.0 * tick_size ||
            std::abs(tob.bid_price - last_bid_price) >= tick_size ||
            std::abs(tob.ask_price - last_ask_price) >= tick_size || quote_age >= max_quote_age;
        if (!should_refresh)
        {
            return orders;
        }

        if (!open_orders.empty() && should_refresh)
        {
            cancel_pending = true;
            return orders;
        }

        const double bid_price = std::round((mid - base_spread / 2.0) / tick_size) * tick_size;
        const double ask_price = std::round((mid + base_spread / 2.0) / tick_size) * tick_size;
        if (bid_price >= ask_price)
        {
            cancel_pending = !open_orders.empty();
            return orders;
        }

        int64_t working_exposure = 0;
        for (const auto& entry : open_orders)
        {
            const StrategyOrder& order = entry.second;
            working_exposure += order.side == Side::Buy ? static_cast<int64_t>(order.quantity)
                                                        : -static_cast<int64_t>(order.quantity);
        }

        const int64_t inventory = position + working_exposure;
        uint64_t buy_quantity = base_order_size;
        uint64_t sell_quantity = base_order_size;
        if (inventory_limit > 0)
        {
            if (inventory > 0)
            {
                buy_quantity = static_cast<uint64_t>(
                    std::max<int64_t>(0, inventory_limit - std::min<int64_t>(inventory, inventory_limit)));
            }
            if (inventory < 0)
            {
                sell_quantity = static_cast<uint64_t>(
                    std::max<int64_t>(0, inventory_limit - std::min<int64_t>(-inventory, inventory_limit)));
            }
        }
        if (inventory > inventory_limit) buy_quantity = 0;
        if (inventory < -inventory_limit) sell_quantity = 0;

        if (buy_quantity > 0)
            orders.emplace_back(Side::Buy, symbol, bid_price, buy_quantity, 0);
        if (sell_quantity > 0)
            orders.emplace_back(Side::Sell, symbol, ask_price, sell_quantity, 0);

        last_quote_mid = mid;
        last_bid_price = tob.bid_price;
        last_ask_price = tob.ask_price;
        quote_age = 0;
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
        if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
        {
            open_orders.erase(report.order_id);
            return;
        }

        if (report.exec_type == ExecType::Trade)
        {
            auto it = open_orders.find(report.order_id);
            if (it != open_orders.end())
            {
                position += report.side == exchange::Side::Buy
                                ? static_cast<int64_t>(report.quantity)
                                : -static_cast<int64_t>(report.quantity);
                it->second.quantity = it->second.quantity > report.quantity ? it->second.quantity - report.quantity : 0;
                if (it->second.quantity == 0) open_orders.erase(it);
            }
        }
    }
};

class InventoryAwareL1MarketMaker : public Strategy
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
    uint64_t quote_age{0};
    uint64_t max_quote_age{20};
    uint64_t quote_cycle{0};
    double inventory_risk_threshold{0.5};

    explicit InventoryAwareL1MarketMaker(uint64_t size = 50, double spd = 0.00003,
                                         int64_t inv_limit = 1000, double ts = 0.00001,
                                         uint64_t max_age = 20,
                                         double risk_threshold = 0.5)
        : base_order_size(size),
          base_spread(spd),
          inventory_limit(inv_limit),
          tick_size(ts),
          max_quote_age(max_age),
          inventory_risk_threshold(risk_threshold)
    {
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
        ++quote_cycle;
        if (!open_orders.empty()) ++quote_age;

        const bool should_refresh = open_orders.empty() ||
                                    std::abs(mid - last_quote_mid) >= 2.0 * tick_size ||
                                    std::abs(tob.bid_price - last_bid_price) >= tick_size ||
                                    std::abs(tob.ask_price - last_ask_price) >= tick_size ||
                                    quote_age >= max_quote_age;
        if (!should_refresh) return orders;

        if (!open_orders.empty() && should_refresh)
        {
            cancel_pending = true;
            return orders;
        }

        const int64_t working_exposure = 0;
        const int64_t inventory = position + working_exposure;
        const double inventory_ratio = inventory_limit > 0
                                          ? std::clamp(static_cast<double>(inventory) /
                                                          static_cast<double>(inventory_limit),
                                                      -1.0, 1.0)
                                          : 0.0;

        double bid_offset = base_spread / 2.0;
        double ask_offset = base_spread / 2.0;
        if (inventory > 0)
        {
            const double pressure = std::clamp(std::abs(inventory_ratio), 0.0, 1.0);
            bid_offset *= (1.0 + pressure * 0.5);
            ask_offset *= (1.0 - pressure * 0.25);
        }
        else if (inventory < 0)
        {
            const double pressure = std::clamp(std::abs(inventory_ratio), 0.0, 1.0);
            ask_offset *= (1.0 + pressure * 0.5);
            bid_offset *= (1.0 - pressure * 0.25);
        }

        const double bid_price = std::round((mid - bid_offset) / tick_size) * tick_size;
        const double ask_price = std::round((mid + ask_offset) / tick_size) * tick_size;
        if (bid_price >= ask_price)
        {
            cancel_pending = !open_orders.empty();
            return orders;
        }

        uint64_t buy_quantity = base_order_size;
        uint64_t sell_quantity = base_order_size;
        if (inventory_limit > 0)
        {
            const double risk_ratio = std::clamp(std::abs(inventory_ratio), 0.0, 1.0);
            if (inventory > 0)
            {
                buy_quantity = static_cast<uint64_t>(
                    std::max<int64_t>(0, base_order_size * (1.0 - risk_ratio * 0.9)));
            }
            else if (inventory < 0)
            {
                sell_quantity = static_cast<uint64_t>(
                    std::max<int64_t>(0, base_order_size * (1.0 - risk_ratio * 0.9)));
            }
        }

        if (inventory > inventory_limit * inventory_risk_threshold) buy_quantity = 0;
        if (inventory < -inventory_limit * inventory_risk_threshold) sell_quantity = 0;

        if (buy_quantity > 0) orders.emplace_back(Side::Buy, symbol, bid_price, buy_quantity, 0);
        if (sell_quantity > 0)
            orders.emplace_back(Side::Sell, symbol, ask_price, sell_quantity, 0);

        last_quote_mid = mid;
        last_bid_price = tob.bid_price;
        last_ask_price = tob.ask_price;
        quote_age = 0;
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
        if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
        {
            open_orders.erase(report.order_id);
            return;
        }

        if (report.exec_type == ExecType::Trade)
        {
            auto it = open_orders.find(report.order_id);
            if (it != open_orders.end())
            {
                position += report.side == exchange::Side::Buy
                                ? static_cast<int64_t>(report.quantity)
                                : -static_cast<int64_t>(report.quantity);
                it->second.quantity = it->second.quantity > report.quantity ? it->second.quantity - report.quantity : 0;
                if (it->second.quantity == 0) open_orders.erase(it);
            }
        }
    }
};

class FlowAwareL1MarketMaker : public Strategy
{
   public:
    struct TradeSample
    {
        Side side;
        uint64_t quantity;
    };

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
    uint64_t quote_age{0};
    uint64_t max_quote_age{20};
    uint64_t quote_cycle{0};
    double inventory_risk_threshold{0.5};
    std::deque<TradeSample> recent_trades;
    uint64_t buy_volume{0};
    uint64_t sell_volume{0};
    std::size_t trade_imbalance_window{32};
    FlowAwareMarketMakerConfig flow_config;

    explicit FlowAwareL1MarketMaker(uint64_t size = 50, double spd = 0.00003,
                                    int64_t inv_limit = 1000, double ts = 0.00001,
                                    uint64_t max_age = 20, double risk_threshold = 0.5,
                                    FlowAwareMarketMakerConfig config = {})
        : base_order_size(size),
          base_spread(spd),
          inventory_limit(inv_limit),
          tick_size(ts),
          max_quote_age(max_age),
          inventory_risk_threshold(risk_threshold),
          flow_config(config)
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
            const auto stale = recent_trades.front();
            recent_trades.pop_front();
            if (stale.side == Side::Buy)
                buy_volume = buy_volume >= stale.quantity ? buy_volume - stale.quantity : 0;
            else
                sell_volume = sell_volume >= stale.quantity ? sell_volume - stale.quantity : 0;
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
        ++quote_cycle;
        if (!open_orders.empty()) ++quote_age;

        const bool should_refresh = open_orders.empty() ||
                                    std::abs(mid - last_quote_mid) >= 2.0 * tick_size ||
                                    std::abs(tob.bid_price - last_bid_price) >= tick_size ||
                                    std::abs(tob.ask_price - last_ask_price) >= tick_size ||
                                    quote_age >= max_quote_age;
        if (!should_refresh) return orders;

        if (!open_orders.empty() && should_refresh)
        {
            cancel_pending = true;
            return orders;
        }

        const int64_t working_exposure = 0;
        const int64_t inventory = position + working_exposure;
        const double inventory_ratio = inventory_limit > 0
                                          ? std::clamp(static_cast<double>(inventory) /
                                                          static_cast<double>(inventory_limit),
                                                      -1.0, 1.0)
                                          : 0.0;
        const double book_imbalance =
            (tob.bid_size + tob.ask_size == 0)
                ? 0.0
                : (static_cast<double>(tob.bid_size) - static_cast<double>(tob.ask_size)) /
                      static_cast<double>(tob.bid_size + tob.ask_size);
        const double total_flow = buy_volume + sell_volume;
        const double trade_imbalance =
            total_flow == 0 ? 0.0 : (static_cast<double>(buy_volume) - static_cast<double>(sell_volume)) /
                                       static_cast<double>(total_flow);
        const double flow_bias_raw = trade_imbalance * flow_config.trade_weight +
                         book_imbalance * flow_config.book_weight;
        const double flow_bias = std::clamp(flow_bias_raw, -0.20, 0.20);

        double bid_offset = base_spread / 2.0;
        double ask_offset = base_spread / 2.0;
        if (inventory > 0)
        {
            const double pressure = std::clamp(std::abs(inventory_ratio), 0.0, 1.0);
            bid_offset *= (1.0 + pressure * 0.5);
            ask_offset *= (1.0 - pressure * 0.25);
        }
        else if (inventory < 0)
        {
            const double pressure = std::clamp(std::abs(inventory_ratio), 0.0, 1.0);
            ask_offset *= (1.0 + pressure * 0.5);
            bid_offset *= (1.0 - pressure * 0.25);
        }

        if (std::abs(flow_bias_raw) >= flow_config.price_threshold)
        {
            if (flow_bias > 0.0)
            {
                ask_offset += static_cast<double>(flow_config.price_ticks) * tick_size;
            }
            else if (flow_bias < 0.0)
            {
                bid_offset += static_cast<double>(flow_config.price_ticks) * tick_size;
            }
        }

        const double bid_price = std::round((mid - bid_offset) / tick_size) * tick_size;
        const double ask_price = std::round((mid + ask_offset) / tick_size) * tick_size;
        if (bid_price >= ask_price)
        {
            cancel_pending = !open_orders.empty();
            return orders;
        }

        uint64_t buy_quantity = base_order_size;
        uint64_t sell_quantity = base_order_size;
        if (inventory_limit > 0)
        {
            const double risk_ratio = std::clamp(std::abs(inventory_ratio), 0.0, 1.0);
            if (inventory > 0)
            {
                buy_quantity = static_cast<uint64_t>(
                    std::max<int64_t>(0, base_order_size * (1.0 - risk_ratio * 0.9)));
            }
            else if (inventory < 0)
            {
                sell_quantity = static_cast<uint64_t>(
                    std::max<int64_t>(0, base_order_size * (1.0 - risk_ratio * 0.9)));
            }
        }
        if (inventory > inventory_limit * inventory_risk_threshold) buy_quantity = 0;
        if (inventory < -inventory_limit * inventory_risk_threshold) sell_quantity = 0;

        if (std::abs(flow_bias_raw) >= flow_config.quantity_threshold)
        {
            const double quantity_reduction =
                std::min(flow_config.max_quantity_reduction, std::abs(flow_bias));
            if (flow_bias > 0.0)
            {
                sell_quantity = static_cast<uint64_t>(std::max<int64_t>(
                    0, std::llround(static_cast<double>(sell_quantity) *
                                    (1.0 - quantity_reduction))));
            }
            else if (flow_bias < 0.0)
            {
                buy_quantity = static_cast<uint64_t>(std::max<int64_t>(
                    0, std::llround(static_cast<double>(buy_quantity) *
                                    (1.0 - quantity_reduction))));
            }
        }

        if (buy_quantity > 0) orders.emplace_back(Side::Buy, symbol, bid_price, buy_quantity, 0);
        if (sell_quantity > 0)
            orders.emplace_back(Side::Sell, symbol, ask_price, sell_quantity, 0);

        last_quote_mid = mid;
        last_bid_price = tob.bid_price;
        last_ask_price = tob.ask_price;
        quote_age = 0;
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
        if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
        {
            open_orders.erase(report.order_id);
            return;
        }

        if (report.exec_type == ExecType::Trade)
        {
            auto it = open_orders.find(report.order_id);
            if (it != open_orders.end())
            {
                position += report.side == exchange::Side::Buy
                                ? static_cast<int64_t>(report.quantity)
                                : -static_cast<int64_t>(report.quantity);
                it->second.quantity = it->second.quantity > report.quantity ? it->second.quantity - report.quantity : 0;
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
    double stress_spread_multiplier{2.5};
    double minimum_stress_quantity_ratio{0.10};
    double maker_fee_rate{0.0002};
    double fee_spread_multiplier{1.0};
    uint64_t max_market_trade_age{1000};
    double max_trade_deviation_bps{50.0};
    uint64_t markout_horizon_quotes{5};
    double flow_trade_weight{0.6};
    double flow_book_weight{0.4};
    double flow_price_threshold{0.06};
    double flow_quantity_threshold{0.08};
    double flow_max_quantity_reduction{0.20};
    uint64_t flow_price_ticks{1};
    uint64_t quote_cycle{0};
    uint64_t submitted_quantity{0};
    uint64_t filled_quantity{0};
    uint64_t fill_count{0};
    uint64_t cancel_count{0};
    uint64_t quote_count{0};
    double fees_paid{0.0};
    double captured_edge{0.0};
    double adverse_selection{0.0};
    uint64_t markout_count{0};
    uint64_t total_quote_lifetime{0};
    uint64_t max_quote_lifetime{0};
    uint64_t inventory_samples{0};
    uint64_t inventory_sign_changes{0};
    int64_t max_abs_inventory{0};
    double average_abs_inventory{0.0};
    struct FillObservation
    {
        Side side;
        double execution_price;
        uint64_t start_cycle;
    };
    std::deque<FillObservation> pending_markouts;
    std::unordered_map<uint64_t, uint64_t> order_start_cycles;
    bool has_inventory_sign{false};
    int inventory_sign{0};

    explicit L1MarketMaker(const L1MarketMakerConfig& config)
        : base_order_size(config.order_size),
          base_spread(config.base_spread),
          inventory_limit(config.inventory_limit),
          tick_size(config.tick_size),
          trade_imbalance_window(config.trade_imbalance_window),
          volatility_window(config.volatility_window),
          max_quote_age(config.max_quote_age),
          max_book_spread_ratio(config.max_book_spread_ratio),
          inventory_risk_threshold(config.inventory_risk_threshold),
          severe_spread_multiplier(config.severe_spread_multiplier),
          stress_spread_multiplier(config.stress_spread_multiplier),
          minimum_stress_quantity_ratio(config.minimum_stress_quantity_ratio),
          maker_fee_rate(config.maker_fee_rate),
          fee_spread_multiplier(config.fee_spread_multiplier),
          max_market_trade_age(config.max_market_trade_age),
          max_trade_deviation_bps(config.max_trade_deviation_bps),
          markout_horizon_quotes(config.markout_horizon_quotes),
          flow_trade_weight(config.flow_trade_weight),
          flow_book_weight(config.flow_book_weight),
          flow_price_threshold(config.flow_price_threshold),
          flow_quantity_threshold(config.flow_quantity_threshold),
          flow_max_quantity_reduction(config.flow_max_quantity_reduction),
          flow_price_ticks(config.flow_price_ticks)
    {
    }

    L1MarketMaker(uint64_t size = 50, double spd = 0.00003, int64_t inv_limit = 1000,
                  double ts = 0.00001)
        : L1MarketMaker(L1MarketMakerConfig{.order_size = size,
                                            .base_spread = spd,
                                            .inventory_limit = inv_limit,
                                            .tick_size = ts})
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

    void on_market_trade(const MarketTrade& trade, const BboQuote* latest_bbo) override
    {
        if (latest_bbo == nullptr || trade.ts < latest_bbo->ts ||
            trade.ts - latest_bbo->ts > max_market_trade_age)
            return;
        const double mid = (latest_bbo->bid_price + latest_bbo->ask_price) / 2.0;
        const double deviation_bps = std::abs(trade.price - mid) / mid * 10000.0;
        if (deviation_bps > max_trade_deviation_bps) return;
        on_market_trade(trade);
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
        ++quote_cycle;
        update_markouts(mid);
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
        record_inventory(inventory);
        const double inventory_ratio =
            inventory_limit > 0
                ? std::clamp(static_cast<double>(inventory) / static_cast<double>(inventory_limit),
                             -1.0, 1.0)
                : 0.0;
        const double inventory_skew = std::tanh(inventory_ratio) * base_spread;
        const double reservation_mid = mid - inventory_skew;
        const double imbalance = trade_imbalance();
        const double book_imbalance = tob_imbalance(tob);
        const double flow_bias = std::clamp(
            imbalance * config_flow_trade_weight() + book_imbalance * config_flow_book_weight(),
            -0.20, 0.20);
        const double adverse_shift = (std::abs(imbalance) * base_spread * 0.5) +
                                     (std::abs(book_imbalance) * base_spread * 0.25);
        const double fee_spread_unit = mid * maker_fee_rate * 2.0;
        const double fee_offset = fee_spread_unit * fee_spread_multiplier / 2.0;
        const double effective_spread = dynamic_spread(tob);

        double raw_bid = reservation_mid - effective_spread / 2.0 - adverse_shift - fee_offset;
        double raw_ask = reservation_mid + effective_spread / 2.0 + adverse_shift + fee_offset;
        if (std::abs(flow_bias) >= flow_price_threshold)
        {
            if (flow_bias > 0.0)
                raw_ask += static_cast<double>(flow_price_ticks) * tick_size;
            else
                raw_bid -= static_cast<double>(flow_price_ticks) * tick_size;
        }
        const double bid_price = std::min(tob.bid_price, round_price(raw_bid));
        const double ask_price = std::max(tob.ask_price, round_price(raw_ask));
        if (bid_price >= ask_price)
        {
            cancel_pending = !open_orders.empty();
            return orders;
        }

        const double stress = spread_stress(effective_spread, market_spread);
        uint64_t buy_quantity = stress_scaled_quantity(stress);
        uint64_t sell_quantity = stress_scaled_quantity(stress);
        if (inventory > 0)
            buy_quantity =
                scaled_quantity(inventory_limit - std::min(inventory, inventory_limit), stress);
        if (inventory < 0)
            sell_quantity =
                scaled_quantity(inventory_limit - std::min(-inventory, inventory_limit), stress);

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

        if (std::abs(flow_bias) >= flow_quantity_threshold)
        {
            const double reduction = std::min(flow_max_quantity_reduction, std::abs(flow_bias));
            if (flow_bias > 0.0)
                sell_quantity = static_cast<uint64_t>(std::llround(sell_quantity * (1.0 - reduction)));
            else
                buy_quantity = static_cast<uint64_t>(std::llround(buy_quantity * (1.0 - reduction)));
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
            ++quote_count;
        }
        return orders;
    }

    void on_order_submitted(const StrategyOrder& order) override
    {
        open_orders[order.order_id] = order;
        order_start_cycles[order.order_id] = quote_cycle;
        submitted_quantity += order.quantity;
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

    StrategyMetrics metrics() const override
    {
        StrategyMetrics result{submitted_quantity,   filled_quantity,       fill_count,
                               cancel_count,         quote_count,           fees_paid,
                               captured_edge,        adverse_selection,     markout_count,
                               total_quote_lifetime, max_quote_lifetime,    average_abs_inventory,
                               max_abs_inventory,    inventory_sign_changes};
        result.available = true;
        return result;
    }

    void on_order_update(const ExecutionReport& report) override
    {
        if (report.exec_type == ExecType::Trade)
        {
            auto it = open_orders.find(report.order_id);
            if (it != open_orders.end())
            {
                const double reference_mid = last_quote_mid;
                position += report.side == exchange::Side::Buy
                                ? static_cast<int64_t>(report.quantity)
                                : -static_cast<int64_t>(report.quantity);
                filled_quantity += report.quantity;
                ++fill_count;
                fees_paid += report.fee;
                captured_edge += report.side == exchange::Side::Buy ? reference_mid - report.price
                                                                    : report.price - reference_mid;
                pending_markouts.push_back(
                    {report.side == exchange::Side::Buy ? Side::Buy : Side::Sell, report.price,
                     quote_cycle});
                it->second.quantity = it->second.quantity > report.quantity
                                          ? it->second.quantity - report.quantity
                                          : 0;
                if (it->second.quantity == 0)
                {
                    record_quote_lifetime(report.order_id);
                    open_orders.erase(it);
                }
            }
        }
        else if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
        {
            if (report.exec_type == ExecType::Cancelled) ++cancel_count;
            record_quote_lifetime(report.order_id);
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

    void record_inventory(int64_t inventory)
    {
        ++inventory_samples;
        const double absolute_inventory = static_cast<double>(std::abs(inventory));
        average_abs_inventory +=
            (absolute_inventory - average_abs_inventory) / static_cast<double>(inventory_samples);
        max_abs_inventory = std::max(max_abs_inventory, std::abs(inventory));

        const int current_sign = inventory > 0 ? 1 : inventory < 0 ? -1 : 0;
        if (has_inventory_sign && current_sign != 0 && current_sign != inventory_sign)
            ++inventory_sign_changes;
        if (current_sign != 0)
        {
            inventory_sign = current_sign;
            has_inventory_sign = true;
        }
    }

    void record_quote_lifetime(uint64_t order_id)
    {
        const auto it = order_start_cycles.find(order_id);
        if (it == order_start_cycles.end()) return;
        const uint64_t lifetime = quote_cycle >= it->second ? quote_cycle - it->second : 0;
        total_quote_lifetime += lifetime;
        max_quote_lifetime = std::max(max_quote_lifetime, lifetime);
        order_start_cycles.erase(it);
    }

    void update_markouts(double current_mid)
    {
        while (!pending_markouts.empty())
        {
            const auto& observation = pending_markouts.front();
            if (quote_cycle < observation.start_cycle + markout_horizon_quotes) break;

            adverse_selection += observation.side == Side::Buy
                                     ? observation.execution_price - current_mid
                                     : current_mid - observation.execution_price;
            ++markout_count;
            pending_markouts.pop_front();
        }
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

    uint64_t scaled_quantity(int64_t available_inventory, double stress = 0.0) const
    {
        if (inventory_limit <= 0 || available_inventory <= 0) return 0;
        const double scale =
            static_cast<double>(available_inventory) / static_cast<double>(inventory_limit);
        const double stress_scale = std::max(minimum_stress_quantity_ratio,
                                             1.0 - stress * (1.0 - minimum_stress_quantity_ratio));
        return static_cast<uint64_t>(std::floor(base_order_size * scale * stress_scale));
    }

    uint64_t stress_scaled_quantity(double stress) const
    {
        const double stress_scale = std::max(minimum_stress_quantity_ratio,
                                             1.0 - stress * (1.0 - minimum_stress_quantity_ratio));
        return static_cast<uint64_t>(std::floor(base_order_size * stress_scale));
    }

    double spread_stress(double effective_spread, double market_spread) const
    {
        if (recent_mid_changes.size() < 2) return 0.0;
        const double average_change =
            std::accumulate(recent_mid_changes.begin(), recent_mid_changes.end(), 0.0) /
            static_cast<double>(recent_mid_changes.size());
        if (average_change <= market_spread) return 0.0;
        const double reference_spread = std::max({base_spread, tick_size, market_spread});
        const double excess = (effective_spread - market_spread) / reference_spread;
        return std::clamp(excess / stress_spread_multiplier, 0.0, 1.0);
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

    double config_flow_trade_weight() const
    {
        return flow_trade_weight;
    }

    double config_flow_book_weight() const
    {
        return flow_book_weight;
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

class ActiveL1MarketMaker : public Strategy
{
   public:
    uint64_t base_order_size;
    double base_spread;
    int64_t inventory_limit;
    double tick_size;
    double maker_fee_rate{0.0002};
    double fee_spread_multiplier{0.30};
    uint64_t max_quote_age{20};
    std::unordered_map<uint64_t, StrategyOrder> open_orders;
    int64_t position{0};
    double last_mid{0.0};
    double last_bid{0.0};
    double last_ask{0.0};
    uint64_t quote_age{0};
    bool cancel_pending{false};
    uint64_t submitted_quantity{0};
    uint64_t filled_quantity{0};
    uint64_t fill_count{0};
    uint64_t cancel_count{0};
    uint64_t quote_count{0};
    double fees_paid{0.0};
    double captured_edge{0.0};
    uint64_t inventory_samples{0};
    uint64_t inventory_sign_changes{0};
    int64_t max_abs_inventory{0};
    double average_abs_inventory{0.0};
    bool has_inventory_sign{false};
    int inventory_sign{0};
    uint64_t total_quote_lifetime{0};
    uint64_t max_quote_lifetime{0};
    std::unordered_map<uint64_t, uint64_t> order_start_cycles;
    uint64_t quote_cycle{0};
    struct FillObservation
    {
        Side side;
        double execution_price;
        uint64_t start_cycle;
    };
    std::deque<FillObservation> pending_markouts;
    double adverse_selection{0.0};
    uint64_t markout_count{0};
    uint64_t markout_horizon{5};
    Side recovery_side{Side::Unknown};
    uint64_t recovery_cycles{0};
    uint64_t recovery_horizon{6};
    struct TradeSample
    {
        Side side;
        uint64_t quantity;
    };
    std::deque<TradeSample> recent_trades;
    uint64_t buy_volume{0};
    uint64_t sell_volume{0};
    std::size_t flow_window{32};

    explicit ActiveL1MarketMaker(uint64_t size = 50, double spd = 0.00003,
                                 int64_t inv_limit = 1000, double ts = 0.00001)
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
        while (recent_trades.size() > flow_window)
        {
            const auto stale = recent_trades.front();
            recent_trades.pop_front();
            if (stale.side == Side::Buy)
                buy_volume = buy_volume >= stale.quantity ? buy_volume - stale.quantity : 0;
            else
                sell_volume = sell_volume >= stale.quantity ? sell_volume - stale.quantity : 0;
        }
    }

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& tob) override
    {
        std::vector<StrategyOrder> orders;
        if (tob.bid_price <= 0.0 || tob.ask_price <= tob.bid_price)
        {
            cancel_pending = !open_orders.empty();
            return orders;
        }

        const double mid = (tob.bid_price + tob.ask_price) / 2.0;
        ++quote_cycle;
        update_markouts(mid);
        if (!open_orders.empty()) ++quote_age;
        const bool price_changed = last_mid == 0.0 || std::abs(mid - last_mid) >= 2.0 * tick_size;
        if (!open_orders.empty() && !price_changed && quote_age < max_quote_age) return orders;
        if (!open_orders.empty())
        {
            cancel_pending = true;
            return orders;
        }

        const int64_t inventory = position + working_exposure();
        record_inventory(inventory);
        if (inventory == 0 || recovery_cycles == 0) recovery_side = Side::Unknown;
        if (recovery_cycles > 0) --recovery_cycles;
        const double inventory_ratio = inventory_limit > 0
                                           ? std::clamp(static_cast<double>(inventory) /
                                                            static_cast<double>(inventory_limit),
                                                        -1.0, 1.0)
                                           : 0.0;
        const double fee_offset = mid * maker_fee_rate * fee_spread_multiplier;
        const double inventory_offset = std::abs(inventory_ratio) * base_spread;
        const double total_flow = static_cast<double>(buy_volume + sell_volume);
        const double trade_imbalance =
            total_flow > 0.0
                ? (static_cast<double>(buy_volume) - static_cast<double>(sell_volume)) /
                      total_flow
                : 0.0;
        const double total_depth = static_cast<double>(tob.bid_size + tob.ask_size);
        const double book_imbalance =
            total_depth > 0.0
                ? (static_cast<double>(tob.bid_size) - static_cast<double>(tob.ask_size)) /
                      total_depth
                : 0.0;
        const double flow_bias = std::clamp(trade_imbalance * 0.6 + book_imbalance * 0.4,
                                            -1.0, 1.0);
        const double flow_offset = std::abs(flow_bias) >= 0.08 ? tick_size : 0.0;

        double bid_price = mid - base_spread / 2.0 - fee_offset;
        double ask_price = mid + base_spread / 2.0 + fee_offset;
        if (inventory > 0)
        {
            bid_price -= inventory_offset * 2.0;
            ask_price -= inventory_offset * 0.5;
        }
        else if (inventory < 0)
        {
            bid_price += inventory_offset * 0.5;
            ask_price += inventory_offset * 2.0;
        }

        if (flow_bias > 0.0)
            ask_price += flow_offset;
        else if (flow_bias < 0.0)
            bid_price -= flow_offset;

        if (recovery_side == Side::Sell)
            ask_price += fee_offset * -0.5;
        else if (recovery_side == Side::Buy)
            bid_price -= fee_offset * -0.5;

        bid_price = std::min(tob.bid_price, round_price(bid_price));
        ask_price = std::max(tob.ask_price, round_price(ask_price));
        if (bid_price >= ask_price) return orders;

        uint64_t buy_quantity = base_order_size;
        uint64_t sell_quantity = base_order_size;
        if (inventory > 0)
        {
            buy_quantity = scaled_quantity(inventory_limit - std::min(inventory, inventory_limit));
            sell_quantity = base_order_size;
        }
        else if (inventory < 0)
        {
            buy_quantity = base_order_size;
            sell_quantity = scaled_quantity(inventory_limit - std::min(-inventory, inventory_limit));
        }

        if (inventory > inventory_limit * 0.8) buy_quantity = 0;
        if (inventory < -inventory_limit * 0.8) sell_quantity = 0;
        if (recovery_side == Side::Buy) sell_quantity = 0;
        if (recovery_side == Side::Sell) buy_quantity = 0;
        if (std::abs(flow_bias) >= 0.08)
        {
            const double flow_reduction = std::min(0.20, std::abs(flow_bias));
            if (flow_bias > 0.0)
                sell_quantity = static_cast<uint64_t>(
                    std::llround(static_cast<double>(sell_quantity) * (1.0 - flow_reduction)));
            else
                buy_quantity = static_cast<uint64_t>(
                    std::llround(static_cast<double>(buy_quantity) * (1.0 - flow_reduction)));
        }
        if (buy_quantity > 0) orders.emplace_back(Side::Buy, symbol, bid_price, buy_quantity, 0);
        if (sell_quantity > 0) orders.emplace_back(Side::Sell, symbol, ask_price, sell_quantity, 0);

        last_mid = mid;
        last_bid = tob.bid_price;
        last_ask = tob.ask_price;
        quote_age = 0;
        if (!orders.empty()) ++quote_count;
        return orders;
    }

    void on_order_submitted(const StrategyOrder& order) override
    {
        open_orders[order.order_id] = order;
        order_start_cycles[order.order_id] = quote_cycle;
        submitted_quantity += order.quantity;
        quote_age = 0;
    }

    std::vector<uint64_t> cancel_requests() override
    {
        if (!cancel_pending) return {};
        std::vector<uint64_t> ids;
        for (const auto& entry : open_orders) ids.push_back(entry.first);
        cancel_pending = false;
        return ids;
    }

    int64_t net_position() const override { return position; }
    size_t working_order_count() const override { return open_orders.size(); }

    StrategyMetrics metrics() const override
    {
        StrategyMetrics result{submitted_quantity,
                               filled_quantity,
                               fill_count,
                               cancel_count,
                               quote_count,
                               fees_paid,
                               captured_edge,
                               adverse_selection,
                               markout_count,
                               total_quote_lifetime,
                               max_quote_lifetime,
                               average_abs_inventory,
                               max_abs_inventory,
                               inventory_sign_changes};
        result.available = true;
        return result;
    }

    void on_order_update(const ExecutionReport& report) override
    {
        if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
        {
            if (report.exec_type == ExecType::Cancelled) ++cancel_count;
            record_quote_lifetime(report.order_id);
            open_orders.erase(report.order_id);
            return;
        }
        if (report.exec_type != ExecType::Trade) return;
        auto it = open_orders.find(report.order_id);
        if (it == open_orders.end()) return;
        position += report.side == exchange::Side::Buy ? static_cast<int64_t>(report.quantity)
                                                        : -static_cast<int64_t>(report.quantity);
        filled_quantity += report.quantity;
        ++fill_count;
        fees_paid += report.fee;
        captured_edge += report.side == exchange::Side::Buy ? last_mid - report.price
                                     : report.price - last_mid;
        pending_markouts.push_back(
            {report.side == exchange::Side::Buy ? Side::Buy : Side::Sell, report.price,
             quote_cycle});
        cancel_pending = true;
        quote_age = max_quote_age;
        recovery_side = report.side == exchange::Side::Buy ? Side::Sell : Side::Buy;
        recovery_cycles = recovery_horizon;
        it->second.quantity = it->second.quantity > report.quantity
                                  ? it->second.quantity - report.quantity
                                  : 0;
        if (it->second.quantity == 0) open_orders.erase(it);
    }

   private:
    void update_markouts(double current_mid)
    {
        while (!pending_markouts.empty())
        {
            const auto& observation = pending_markouts.front();
            if (quote_cycle < observation.start_cycle + markout_horizon) break;
            adverse_selection += observation.side == Side::Buy
                                     ? observation.execution_price - current_mid
                                     : current_mid - observation.execution_price;
            ++markout_count;
            pending_markouts.pop_front();
        }
    }

    void record_inventory(int64_t inventory)
    {
        ++inventory_samples;
        const double absolute_inventory = static_cast<double>(std::abs(inventory));
        average_abs_inventory +=
            (absolute_inventory - average_abs_inventory) / static_cast<double>(inventory_samples);
        max_abs_inventory = std::max(max_abs_inventory, std::abs(inventory));
        const int current_sign = inventory > 0 ? 1 : inventory < 0 ? -1 : 0;
        if (has_inventory_sign && current_sign != 0 && current_sign != inventory_sign)
            ++inventory_sign_changes;
        if (current_sign != 0)
        {
            inventory_sign = current_sign;
            has_inventory_sign = true;
        }
    }

    void record_quote_lifetime(uint64_t order_id)
    {
        const auto it = order_start_cycles.find(order_id);
        if (it == order_start_cycles.end()) return;
        const uint64_t lifetime = quote_cycle >= it->second ? quote_cycle - it->second : 0;
        total_quote_lifetime += lifetime;
        max_quote_lifetime = std::max(max_quote_lifetime, lifetime);
        order_start_cycles.erase(it);
    }

    int64_t working_exposure() const
    {
        int64_t exposure = 0;
        for (const auto& entry : open_orders)
            exposure += entry.second.side == Side::Buy
                            ? static_cast<int64_t>(entry.second.quantity)
                            : -static_cast<int64_t>(entry.second.quantity);
        return exposure;
    }

    uint64_t scaled_quantity(int64_t available) const
    {
        if (inventory_limit <= 0 || available <= 0) return 0;
        return static_cast<uint64_t>(std::min<int64_t>(
            base_order_size, std::max<int64_t>(0, available)));
    }

    double round_price(double price) const
    {
        return std::round(price / tick_size) * tick_size;
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
