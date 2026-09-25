#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "exchange/execution_report.h"
#include "strategy/strategy.h"

enum class L2SignalMode
{
    Baseline,
    Depth,
    Micro,
    Flow
};

struct L2MarketMakerConfig
{
    uint64_t order_size{1};
    double base_spread{1.0};
    int64_t inventory_limit{100};
    double tick_size{0.5};
    double imbalance_shift{1.0};
    double trade_imbalance_shift{1.0};
    L2SignalMode signal_mode{L2SignalMode::Depth};
    uint64_t trade_window{32};
    uint64_t refresh_price_ticks{1};
    uint64_t max_quote_age{20};
    double toxicity_flow_threshold{2.0};
    double weak_flow_threshold{0.9};
    double weak_flow_quote_scale{0.5};
    double weak_flow_spread_shift_ticks{1.0};
};

class L2MarketMaker final : public Strategy
{
   public:
    explicit L2MarketMaker(L2MarketMakerConfig config = {}) : config_(config) {}

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& top) override
    {
        const double mid = (top.bid_price + top.ask_price) / 2.0;
        return quote(symbol, top, mid);
    }

    std::vector<StrategyOrder> on_l2_market_view(const L2MarketView& view) override
    {
        if (view.top.bid_price <= 0.0 || view.top.ask_price <= 0.0)
            return quote(view.symbol, view.top, 0.0);

        const double mid = (view.top.bid_price + view.top.ask_price) / 2.0;
        double fair_price = mid;
        if (config_.signal_mode == L2SignalMode::Depth)
            fair_price += view.depth_imbalance * config_.imbalance_shift;
        else if (config_.signal_mode == L2SignalMode::Micro)
            fair_price = view.micro_price > 0.0 ? view.micro_price : mid;
        else if (config_.signal_mode == L2SignalMode::Flow)
        {
            const double micro_price = view.micro_price > 0.0 ? view.micro_price : mid;
            fair_price = micro_price + view.depth_imbalance * config_.imbalance_shift +
                         trade_imbalance() * config_.trade_imbalance_shift;
        }
        return quote(view.symbol, view.top, fair_price);
    }

    void on_market_trade(const MarketTrade& trade) override
    {
        if (trade.aggressor_side != Side::Buy && trade.aggressor_side != Side::Sell) return;
        recent_trades_.push_back({trade.aggressor_side, trade.quantity});
        if (trade.aggressor_side == Side::Buy)
            buy_volume_ += trade.quantity;
        else
            sell_volume_ += trade.quantity;
        while (recent_trades_.size() > config_.trade_window)
        {
            const auto stale_trade = recent_trades_.front();
            recent_trades_.pop_front();
            if (stale_trade.side == Side::Buy)
                buy_volume_ = buy_volume_ > stale_trade.quantity
                                   ? buy_volume_ - stale_trade.quantity
                                   : 0;
            else
                sell_volume_ = sell_volume_ > stale_trade.quantity
                                   ? sell_volume_ - stale_trade.quantity
                                   : 0;
        }
    }

    void on_order_submitted(const StrategyOrder& order) override
    {
        open_orders_[order.order_id] = order;
        metrics_.submitted_quantity += order.quantity;
        ++metrics_.quote_count;
    }

    std::vector<uint64_t> cancel_requests() override
    {
        if (!cancel_pending_) return {};
        std::vector<uint64_t> order_ids;
        order_ids.reserve(open_orders_.size());
        for (const auto& entry : open_orders_) order_ids.push_back(entry.first);
        cancel_pending_ = false;
        metrics_.cancel_count += order_ids.size();
        return order_ids;
    }

    void request_cancel_all()
    {
        cancel_pending_ = !open_orders_.empty();
    }

    int64_t net_position() const override
    {
        return position_;
    }

    std::size_t working_order_count() const override
    {
        return open_orders_.size();
    }

    StrategyMetrics metrics() const override
    {
        auto result = metrics_;
        result.available = true;
        return result;
    }

    void on_order_update(const ExecutionReport& report) override
    {
        if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
        {
            open_orders_.erase(report.order_id);
            return;
        }
        if (report.exec_type != ExecType::Trade) return;

        auto order = open_orders_.find(report.order_id);
        if (order == open_orders_.end()) return;
        position_ += report.side == exchange::Side::Buy
                         ? static_cast<int64_t>(report.quantity)
                         : -static_cast<int64_t>(report.quantity);
        metrics_.filled_quantity += report.quantity;
        ++metrics_.fill_count;
        metrics_.fees_paid += report.fee;
        order->second.quantity = order->second.quantity > report.quantity
                                     ? order->second.quantity - report.quantity
                                     : 0;
        if (order->second.quantity == 0) open_orders_.erase(order);
    }

   private:
    double trade_imbalance() const
    {
        const uint64_t total_volume = buy_volume_ + sell_volume_;
        if (total_volume == 0) return 0.0;
        return static_cast<double>(buy_volume_) / static_cast<double>(total_volume) * 2.0 - 1.0;
    }

    std::vector<StrategyOrder> quote(const std::string& symbol, const TopOfBook& top,
                                     double fair_price)
    {
        std::vector<StrategyOrder> orders;
        if (top.bid_price <= 0.0 || top.ask_price <= 0.0 || top.bid_price >= top.ask_price ||
            config_.order_size == 0 || config_.tick_size <= 0.0)
        {
            cancel_pending_ = !open_orders_.empty();
            return orders;
        }

        if (!open_orders_.empty())
        {
            ++quote_age_;
            const bool price_changed = last_fair_price_ <= 0.0 ||
                                       std::abs(fair_price - last_fair_price_) >=
                                           static_cast<double>(config_.refresh_price_ticks) *
                                               config_.tick_size;
            if (quote_age_ < config_.max_quote_age && !price_changed) return orders;
            if (price_changed)
                ++metrics_.price_refresh_count;
            else
                ++metrics_.age_refresh_count;
            cancel_pending_ = true;
            return orders;
        }

        if (fair_price <= 0.0) fair_price = (top.bid_price + top.ask_price) / 2.0;

        int64_t working_position = position_;
        const double inventory_ratio = config_.inventory_limit > 0
                                           ? std::clamp(static_cast<double>(working_position) /
                                                            static_cast<double>(config_.inventory_limit),
                                                        -1.0, 1.0)
                                           : 0.0;
        const double inventory_shift = inventory_ratio * config_.base_spread;
        double weak_flow_spread_shift = 0.0;
        uint64_t buy_quantity = config_.order_size;
        uint64_t sell_quantity = config_.order_size;
        if (working_position >= config_.inventory_limit) buy_quantity = 0;
        if (working_position <= -config_.inventory_limit) sell_quantity = 0;
        if (config_.signal_mode == L2SignalMode::Flow && config_.toxicity_flow_threshold <= 1.0)
        {
            const double flow = trade_imbalance();
            const double flow_abs = std::abs(flow);
            if (flow > config_.toxicity_flow_threshold) sell_quantity = 0;
            if (flow < -config_.toxicity_flow_threshold) buy_quantity = 0;
            if (flow_abs > config_.weak_flow_threshold && std::abs(inventory_ratio) > 0.25)
            {
                weak_flow_spread_shift = config_.weak_flow_spread_shift_ticks * config_.tick_size;
                const double weak_scale = std::max(0.0, config_.weak_flow_quote_scale);
                buy_quantity = static_cast<uint64_t>(std::max(
                    0.0, static_cast<double>(buy_quantity) * weak_scale));
                sell_quantity = static_cast<uint64_t>(std::max(
                    0.0, static_cast<double>(sell_quantity) * weak_scale));
            }
        }
        const double raw_bid_price = std::floor(
                                         (fair_price - config_.base_spread - inventory_shift -
                                          weak_flow_spread_shift) /
                                         config_.tick_size) *
                                     config_.tick_size;
        const double raw_ask_price = std::ceil(
                                         (fair_price + config_.base_spread - inventory_shift +
                                          weak_flow_spread_shift) /
                                         config_.tick_size) *
                                     config_.tick_size;
        const double passive_bid =
            std::floor(top.bid_price / config_.tick_size) * config_.tick_size;
        const double passive_ask =
            std::ceil(top.ask_price / config_.tick_size) * config_.tick_size;
        const double bid_price = std::min(raw_bid_price, passive_bid);
        const double ask_price = std::max(raw_ask_price, passive_ask);
        if (!std::isfinite(bid_price) || !std::isfinite(ask_price) || bid_price >= ask_price)
            return orders;

        if (buy_quantity > 0) orders.emplace_back(Side::Buy, symbol, bid_price, buy_quantity, 0);
        if (sell_quantity > 0) orders.emplace_back(Side::Sell, symbol, ask_price, sell_quantity, 0);
        if (!orders.empty())
        {
            last_fair_price_ = fair_price;
            quote_age_ = 0;
        }
        return orders;
    }

    struct TradeVolume
    {
        Side side;
        uint64_t quantity;
    };

    L2MarketMakerConfig config_;
    std::unordered_map<uint64_t, StrategyOrder> open_orders_;
    std::deque<TradeVolume> recent_trades_;
    uint64_t buy_volume_{0};
    uint64_t sell_volume_{0};
    int64_t position_{0};
    bool cancel_pending_{false};
    uint64_t quote_age_{0};
    double last_fair_price_{0.0};
    StrategyMetrics metrics_;
};
