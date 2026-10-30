#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "exchange/execution_report.h"
#include "strategy/strategy.h"

/*
-------------------------------------------
 ExecutionL2Strategy
-------------------------------------------

The execution line, the mirror image of the signal line: there is no view about direction at all. A parent
order -- a side, a quantity and a deadline -- arrives from the run's configuration, and the job is to work it
out as cheaply as the venue allows. The children cross the touch and are sent with a time in force that
cancels the remainder, so this file is about a schedule rather than about a queue.

Three schedules, one interface:

  Market  one child for the whole parent, sent on the first usable view. It is the control: a single crossing
          is the cost the other two are measured against.
  Twap    `slices` equal children spaced evenly across the horizon.
  Pov     children sized to `participation` of the volume that has actually traded since the parent started,
          so the order follows the market's own pace instead of a clock.

The horizon is market time, not a view count: a view is 7 ms on the recorded slices and a different number on
any other feed, and a schedule that meant "ten views" would mean something different on each.
-------------------------------------------
*/

enum class ExecutionAlgo : uint8_t
{
    Market = 0,
    Twap,
    Pov
};

struct ExecutionL2Config
{
    // The parent. A side of Unknown or a zero quantity is an inert strategy, which app_config refuses to
    // build: an execution run without a parent has nothing to execute.
    Side side{Side::Unknown};
    uint64_t quantity{0};
    uint64_t horizon_us{0};
    ExecutionAlgo algo{ExecutionAlgo::Market};
    // TWAP: the number of equal children. POV: the share of traded volume to follow.
    uint64_t slices{10};
    double participation{0.10};
    double tick_size{0.5};
};

class ExecutionL2Strategy : public Strategy
{
   public:
    explicit ExecutionL2Strategy(ExecutionL2Config config = {}) : config_(config) {}

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
    void on_market_trade(const MarketTrade& trade) override;
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

    // What the schedule has done so far, for the unit test and for a reader of the summary.
    uint64_t submitted_quantity() const
    {
        return submitted_quantity_;
    }

    uint64_t filled_quantity() const
    {
        return filled_quantity_;
    }

    uint64_t child_orders() const
    {
        return child_orders_;
    }

   private:
    struct WorkingChild
    {
        uint64_t submitted_ts{0};
        uint64_t quantity{0};
    };

    // What is still owed to the parent: the quantity less what has filled and what is still working. A child
    // that dies unfilled leaves this number where it was, so the schedule retries rather than silently
    // declaring the parent done.
    uint64_t remaining_quantity() const;
    bool usable(const TopOfBook& top) const;
    double child_price(const TopOfBook& top) const;
    uint64_t slice_quantity() const;
    uint64_t pov_quantity() const;
    void request_stale_cancels();

    ExecutionL2Config config_;
    bool started_{false};
    bool sent_market_child_{false};
    uint64_t start_ts_{0};
    uint64_t end_ts_{0};
    uint64_t next_slice_ts_{0};
    uint64_t filled_quantity_{0};
    uint64_t submitted_quantity_{0};
    uint64_t child_orders_{0};
    uint64_t market_volume_{0};
    uint64_t last_view_ts_{0};
    int64_t position_{0};
    std::unordered_map<uint64_t, WorkingChild> open_orders_;
    std::vector<uint64_t> cancel_queue_;
    StrategyMetrics metrics_{};
};

inline uint64_t ExecutionL2Strategy::remaining_quantity() const
{
    uint64_t working = 0;
    for (const auto& [order_id, child] : open_orders_) working += child.quantity;
    const uint64_t committed = filled_quantity_ + working;
    return config_.quantity > committed ? config_.quantity - committed : 0;
}

inline bool ExecutionL2Strategy::usable(const TopOfBook& top) const
{
    return top.bid_price > 0.0 && top.ask_price > 0.0;
}

inline double ExecutionL2Strategy::child_price(const TopOfBook& top) const
{
    // Buying takes the offer and selling hits the bid: the child is a crossing order by construction, which is
    // what makes its cost the thing this line measures.
    return config_.side == Side::Buy ? top.ask_price : top.bid_price;
}

inline uint64_t ExecutionL2Strategy::slice_quantity() const
{
    const uint64_t slices = std::max<uint64_t>(1, config_.slices);
    const uint64_t even = config_.quantity / slices;
    return std::max<uint64_t>(1, even);
}

inline uint64_t ExecutionL2Strategy::pov_quantity() const
{
    // A share of the volume that has traded since the parent started, less what has already filled; a child
    // that died unfilled leaves the allowance intact rather than consuming it.
    const double target = config_.participation * static_cast<double>(market_volume_);
    const double allowance = target - static_cast<double>(filled_quantity_);
    return allowance >= 1.0 ? static_cast<uint64_t>(allowance) : 0;
}

inline void ExecutionL2Strategy::request_stale_cancels()
{
    // A child carries IOC, so it should never rest. If one is still open after the view it was sent on, the
    // venue (or a time-in-force override) left it resting, and the schedule pulls it rather than working a
    // second child on top of the first.
    for (const auto& [order_id, child] : open_orders_)
        if (child.submitted_ts < last_view_ts_) cancel_queue_.push_back(order_id);
}

inline std::vector<StrategyOrder> ExecutionL2Strategy::on_l2_market_view(const L2MarketView& view)
{
    last_view_ts_ = view.ts;
    request_stale_cancels();

    std::vector<StrategyOrder> orders;
    if (config_.side == Side::Unknown || config_.quantity == 0 || !usable(view.top)) return orders;
    if (!started_)
    {
        started_ = true;
        start_ts_ = view.ts;
        end_ts_ = view.ts + std::max<uint64_t>(1, config_.horizon_us);
        next_slice_ts_ = view.ts;
    }
    if (view.ts > end_ts_ || filled_quantity_ >= config_.quantity) return orders;

    uint64_t quantity = 0;
    switch (config_.algo)
    {
        case ExecutionAlgo::Market:
            if (!sent_market_child_)
            {
                sent_market_child_ = true;
                quantity = remaining_quantity();
            }
            break;
        case ExecutionAlgo::Twap:
            if (view.ts >= next_slice_ts_)
            {
                quantity = std::min(slice_quantity(), remaining_quantity());
                // Advance by the slice interval rather than by a view count, so a slow view rate stretches
                // the schedule instead of piling several slices onto one view.
                const uint64_t slices = std::max<uint64_t>(1, config_.slices);
                const uint64_t interval = std::max<uint64_t>(1, config_.horizon_us / slices);
                next_slice_ts_ = view.ts + interval;
            }
            break;
        case ExecutionAlgo::Pov:
            quantity = std::min(pov_quantity(), remaining_quantity());
            break;
    }
    if (quantity == 0) return orders;

    // A child never exceeds what the parent still owes, and it is always an IOC at the touch.
    orders.emplace_back(config_.side, view.symbol, child_price(view.top), quantity, 0,
                        TimeInForce::Ioc);
    return orders;
}

inline void ExecutionL2Strategy::on_market_trade(const MarketTrade& trade)
{
    // POV follows the market's own pace, so it needs the venue's printed volume rather than a clock. Trades
    // before the parent starts belong to somebody else's order and are not counted.
    if (!started_) return;
    market_volume_ += trade.quantity;
}

inline void ExecutionL2Strategy::on_order_submitted(const StrategyOrder& order)
{
    open_orders_.insert_or_assign(order.order_id, WorkingChild{last_view_ts_, order.quantity});
    ++child_orders_;
    submitted_quantity_ += order.quantity;
    ++metrics_.quote_count;
    metrics_.submitted_quantity += order.quantity;
}

inline std::vector<uint64_t> ExecutionL2Strategy::cancel_requests()
{
    auto ids = cancel_queue_;
    cancel_queue_.clear();
    metrics_.cancel_count += ids.size();
    return ids;
}

inline void ExecutionL2Strategy::on_order_update(const ExecutionReport& report)
{
    if (report.exec_type == ExecType::Trade)
    {
        const int64_t executed = static_cast<int64_t>(report.executed_quantity());
        position_ += report.side == exchange::Side::Buy ? executed : -executed;
        filled_quantity_ += report.executed_quantity();
        ++metrics_.fill_count;
        metrics_.filled_quantity += report.executed_quantity();
        metrics_.fees_paid += report.fee;
    }
    if (is_terminal(report.exec_type)) open_orders_.erase(report.order_id);
}
