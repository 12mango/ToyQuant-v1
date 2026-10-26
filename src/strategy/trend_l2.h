#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "exchange/execution_report.h"
#include "strategy/strategy.h"

/*
-------------------------------------------
 TrendL2Strategy
-------------------------------------------

The non-market-making line, kept deliberately small: a signal, a target position, a clip at the touch, and
three ways out. It exists to walk the workflow the market makers do not -- signal, then target, then risk,
then exit -- on the same pipeline, book and engine.

The signal is an EWMA cross: a fast and a slow average of the midpoint. When the fast one is above the slow
one by `entry_ticks`, the target is the long limit; below it by the same distance, the short limit; when the
gap falls inside `exit_ticks` of zero, the target is flat. Nothing is decided until the slow average has seen
`slow_views` views, so the first decisions are not made on an average that is still just the first price.

The position is reached in clips of `order_size` at the opposite touch, which the engine fills as a taker.
A working order is left alone until it fills, the touch moves away, or the target changes; each of those is a
cancel and the next view sends the next clip. The target is a position, not a quote, so there is no queue
model to argue about here.

Three exits sit above the signal. A stop when the midpoint has moved `stop_ticks` against the entry, a time
stop after `max_holding_views`, and the signal's own exit band. The first two raise `risk_pause_count`, which
is the same field the maker uses for a risk pause, and start a cooldown during which no new position is
opened; without it a stop on a still-strong signal is an exit followed by the same entry on the next view.
-------------------------------------------
*/

struct TrendL2Config
{
    uint64_t order_size{1};
    // The target size of a full signal, in engine units. The position is built in clips of order_size.
    int64_t max_position{100};
    double tick_size{0.5};
    // The two averages, in views. The slow one also gates the warmup.
    uint64_t fast_views{10};
    uint64_t slow_views{60};
    // The EWMA gap that opens a position and the one that closes it, in ticks.
    double entry_ticks{2.0};
    double exit_ticks{0.5};
    // A stop against the volume-weighted entry, and a time stop, both in the same units the run reports.
    double stop_ticks{6.0};
    uint64_t max_holding_views{2000};
    // Views after a stop or a time stop during which no new position is opened. Without it a stop on a
    // still-strong signal is an exit followed by the same entry on the next view, which is not a stop.
    uint64_t exit_cooldown_views{60};
};

class TrendL2Strategy : public Strategy
{
   public:
    explicit TrendL2Strategy(TrendL2Config config = {}) : config_(config) {}

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& top) override
    {
        L2MarketView view;
        view.symbol = symbol;
        view.top = top;
        view.micro_price = (top.bid_price + top.ask_price) / 2.0;
        return on_l2_market_view(view);
    }

    std::vector<StrategyOrder> on_l2_market_view(const L2MarketView& view) override;
    void on_order_submitted(const StrategyOrder& order) override;
    std::vector<uint64_t> cancel_requests() override;
    void on_order_update(const ExecutionReport& report) override;

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

    int64_t target_position() const
    {
        return target_;
    }

   private:
    void update_signal(double mid);
    void update_target(double mid);
    bool stop_hit(double mid) const;
    void request_cancel_all();

    TrendL2Config config_;
    double fast_mid_{0.0};
    double slow_mid_{0.0};
    bool warm_{false};
    uint64_t views_{0};
    int64_t position_{0};
    int64_t target_{0};
    double entry_price_{0.0};
    uint64_t entry_view_{0};
    uint64_t cooldown_left_{0};
    std::unordered_map<uint64_t, StrategyOrder> open_orders_;
    int64_t target_at_submit_{0};
    bool cancel_pending_{false};
    StrategyMetrics metrics_{};
};

inline void TrendL2Strategy::update_signal(double mid)
{
    const double fast_alpha =
        2.0 / (static_cast<double>(std::max<uint64_t>(1, config_.fast_views)) + 1.0);
    const double slow_alpha =
        2.0 / (static_cast<double>(std::max<uint64_t>(1, config_.slow_views)) + 1.0);
    if (views_ <= 1)
    {
        fast_mid_ = mid;
        slow_mid_ = mid;
    }
    else
    {
        fast_mid_ += fast_alpha * (mid - fast_mid_);
        slow_mid_ += slow_alpha * (mid - slow_mid_);
    }
    if (views_ >= std::max<uint64_t>(1, config_.slow_views)) warm_ = true;
}

inline bool TrendL2Strategy::stop_hit(double mid) const
{
    if (entry_price_ <= 0.0 || config_.stop_ticks <= 0.0) return false;
    const double distance = config_.stop_ticks * config_.tick_size;
    return position_ > 0 ? mid <= entry_price_ - distance : mid >= entry_price_ + distance;
}

inline void TrendL2Strategy::update_target(double mid)
{
    const double tick = config_.tick_size > 0.0 ? config_.tick_size : 1.0;
    const double trend_ticks = (fast_mid_ - slow_mid_) / tick;
    const int64_t max_position = std::max<int64_t>(0, config_.max_position);

    // The two risk exits come before the signal, so a stop or a time stop cannot be argued away by a
    // trend that is still pointing the other way.
    if (position_ != 0)
    {
        if (stop_hit(mid))
        {
            if (target_ != 0)
            {
                ++metrics_.risk_pause_count;
                cooldown_left_ = config_.exit_cooldown_views;
            }
            target_ = 0;
            return;
        }
        if (entry_view_ != 0 && views_ - entry_view_ >= config_.max_holding_views)
        {
            if (target_ != 0)
            {
                ++metrics_.risk_pause_count;
                cooldown_left_ = config_.exit_cooldown_views;
            }
            target_ = 0;
            return;
        }
    }

    // The cooldown holds the target flat; the order path below still reduces an open position to it.
    if (cooldown_left_ > 0)
    {
        --cooldown_left_;
        target_ = 0;
        return;
    }

    if (!warm_)
    {
        target_ = 0;
        return;
    }
    if (target_ == 0)
    {
        if (trend_ticks >= config_.entry_ticks) target_ = max_position;
        else if (trend_ticks <= -config_.entry_ticks) target_ = -max_position;
    }
    else if ((target_ > 0 && trend_ticks <= config_.exit_ticks) ||
             (target_ < 0 && trend_ticks >= -config_.exit_ticks))
    {
        target_ = 0;
    }
}

inline void TrendL2Strategy::request_cancel_all()
{
    cancel_pending_ = !open_orders_.empty();
}

inline std::vector<StrategyOrder> TrendL2Strategy::on_l2_market_view(const L2MarketView& view)
{
    const double mid = (view.top.bid_price + view.top.ask_price) / 2.0;
    if (view.top.bid_price <= 0.0 || view.top.ask_price <= 0.0 ||
        view.top.bid_price >= view.top.ask_price || config_.tick_size <= 0.0)
    {
        request_cancel_all();
        return {};
    }

    ++views_;
    update_signal(mid);
    update_target(mid);

    // A target change, or a touch that moved away from a working order, is a cancel. The next view
    // sends the next clip after the cancel has been acknowledged; sending both on one view would
    // double the quantity the pipeline has to reserve.
    if (!open_orders_.empty())
    {
        if (target_ != target_at_submit_) request_cancel_all();
        for (const auto& entry : open_orders_)
        {
            const StrategyOrder& order = entry.second;
            const double touch = order.side == Side::Buy ? view.top.ask_price : view.top.bid_price;
            if (std::abs(order.price - touch) >= config_.tick_size)
            {
                request_cancel_all();
                break;
            }
        }
    }
    if (cancel_pending_) return {};

    std::vector<StrategyOrder> orders;
    if (!open_orders_.empty() || position_ == target_) return orders;

    const bool buy = target_ > position_;
    const int64_t difference = std::llabs(target_ - position_);
    const uint64_t quantity =
        std::min<uint64_t>(config_.order_size, static_cast<uint64_t>(difference));
    if (quantity == 0) return orders;

    const double price = buy ? view.top.ask_price : view.top.bid_price;
    orders.emplace_back(buy ? Side::Buy : Side::Sell, view.symbol, price, quantity, 0);
    return orders;
}

inline void TrendL2Strategy::on_order_submitted(const StrategyOrder& order)
{
    open_orders_[order.order_id] = order;
    target_at_submit_ = target_;
    metrics_.submitted_quantity += order.quantity;
    ++metrics_.quote_count;
    if (order.side == Side::Buy) ++metrics_.buy_quote_count;
    else if (order.side == Side::Sell) ++metrics_.sell_quote_count;
}

inline std::vector<uint64_t> TrendL2Strategy::cancel_requests()
{
    if (!cancel_pending_) return {};
    std::vector<uint64_t> order_ids;
    order_ids.reserve(open_orders_.size());
    for (const auto& entry : open_orders_) order_ids.push_back(entry.first);
    cancel_pending_ = false;
    metrics_.cancel_count += order_ids.size();
    return order_ids;
}

inline void TrendL2Strategy::on_order_update(const ExecutionReport& report)
{
    auto order = open_orders_.find(report.order_id);
    if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
    {
        open_orders_.erase(report.order_id);
        return;
    }
    if (report.exec_type != ExecType::Trade || order == open_orders_.end()) return;

    const uint64_t quantity = report.executed_quantity();
    if (quantity == 0) return;

    const bool buy = report.side == exchange::Side::Buy;
    const int64_t signed_quantity =
        buy ? static_cast<int64_t>(quantity) : -static_cast<int64_t>(quantity);
    const int64_t previous = position_;
    position_ += signed_quantity;

    if (previous == 0)
    {
        entry_price_ = report.price;
        entry_view_ = views_;
    }
    else if ((previous > 0 && signed_quantity > 0) || (previous < 0 && signed_quantity < 0))
    {
        const double previous_size = static_cast<double>(std::llabs(previous));
        const double added = static_cast<double>(quantity);
        entry_price_ = (entry_price_ * previous_size + report.price * added) /
                       (previous_size + added);
    }
    else if ((previous > 0 && position_ < 0) || (previous < 0 && position_ > 0))
    {
        entry_price_ = report.price;
        entry_view_ = views_;
    }
    if (position_ == 0)
    {
        entry_price_ = 0.0;
        entry_view_ = 0;
        target_ = 0;
    }

    metrics_.filled_quantity += quantity;
    ++metrics_.fill_count;
    metrics_.fees_paid += report.fee;
    if (buy) ++metrics_.buy_fill_count;
    else ++metrics_.sell_fill_count;

    if (order->second.quantity > quantity) order->second.quantity -= quantity;
    else open_orders_.erase(order);
}
