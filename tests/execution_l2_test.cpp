#include <cassert>
#include <cmath>
#include <cstdint>

#include "strategy/execution_l2.h"

namespace
{
L2MarketView view_at(uint64_t ts, double mid, uint64_t size = 1000)
{
    L2MarketView view;
    view.symbol = "X";
    view.ts = ts;
    view.top = TopOfBook{mid - 0.5, size, mid + 0.5, size};
    return view;
}

ExecutionL2Config parent(ExecutionAlgo algo, uint64_t quantity = 100, uint64_t horizon_s = 10)
{
    return ExecutionL2Config{.side = Side::Buy,
                             .quantity = quantity,
                             .horizon_us = horizon_s * 1'000'000,
                             .algo = algo,
                             .slices = 10,
                             .participation = 0.1,
                             .tick_size = 1.0};
}

// Deliver what the pipeline would: the order is submitted, its fill is reported, and the terminal report that
// closes it follows. Without the terminal report the child would look like it is still working, and the
// schedule would correctly stop submitting — which is a state the venue never leaves us in.
void fill(ExecutionL2Strategy& strategy, const StrategyOrder& order)
{
    strategy.on_order_submitted(order);
    ExecutionReport report;
    report.exec_type = ExecType::Trade;
    report.side = order.side == Side::Buy ? exchange::Side::Buy : exchange::Side::Sell;
    report.price = order.price;
    report.quantity = order.quantity;
    strategy.on_order_update(report);
    report.exec_type = ExecType::Filled;
    report.quantity = 0;
    strategy.on_order_update(report);
}
}  // namespace

int main()
{
    // Market: one child for the whole parent, at the offer, with a time in force that cancels the remainder.
    {
        ExecutionL2Strategy strategy(parent(ExecutionAlgo::Market));
        const auto first = strategy.on_l2_market_view(view_at(1'000'000, 100.0));
        assert(first.size() == 1);
        assert(first[0].side == Side::Buy);
        assert(first[0].price == 100.5);
        assert(first[0].quantity == 100);
        assert(first[0].time_in_force == TimeInForce::Ioc);
        fill(strategy, first[0]);
        // Nothing left to do: the parent is complete.
        assert(strategy.on_l2_market_view(view_at(2'000'000, 100.0)).empty());
        assert(strategy.filled_quantity() == 100);
    }

    // Twap: ten children of ten across a ten-second horizon, and none after the deadline.
    {
        ExecutionL2Strategy strategy(parent(ExecutionAlgo::Twap));
        uint64_t children = 0;
        uint64_t total = 0;
        for (uint64_t second = 1; second <= 14; ++second)
        {
            for (const auto& order : strategy.on_l2_market_view(view_at(second * 1'000'000, 100.0)))
            {
                ++children;
                assert(order.quantity == 10);
                total += order.quantity;
                fill(strategy, order);
            }
        }
        assert(children == 10);
        assert(total == 100);
        assert(strategy.child_orders() == 10);
    }

    // Pov: sized to a share of the volume that has traded since the parent started, not to a clock.
    {
        ExecutionL2Strategy strategy(parent(ExecutionAlgo::Pov));
        assert(strategy.on_l2_market_view(view_at(1'000'000, 100.0)).empty());
        // 1,000 contracts print, so 10% is 100: one child for the whole parent.
        strategy.on_market_trade(MarketTrade{1'100'000, "X", 100.0, 1000, Side::Sell, 1, "x"});
        const auto orders = strategy.on_l2_market_view(view_at(1'200'000, 100.0));
        assert(orders.size() == 1);
        assert(orders[0].quantity == 100);
    }

    // A child that is still open after the view it was sent on is pulled on the next view, so a venue that
    // ignores the time in force cannot leave two children working at once.
    {
        ExecutionL2Strategy strategy(parent(ExecutionAlgo::Market));
        const auto first = strategy.on_l2_market_view(view_at(1'000'000, 100.0));
        assert(first.size() == 1);
        strategy.on_order_submitted(first[0]);
        const auto cancels = strategy.cancel_requests();
        assert(cancels.empty());
        strategy.on_l2_market_view(view_at(2'000'000, 100.0));
        const auto stale = strategy.cancel_requests();
        assert(stale.size() == 1);
        assert(stale[0] == first[0].order_id);
    }

    // Without a parent the strategy is inert rather than guessing one.
    {
        ExecutionL2Strategy strategy(ExecutionL2Config{});
        assert(strategy.on_l2_market_view(view_at(1'000'000, 100.0)).empty());
    }
}
