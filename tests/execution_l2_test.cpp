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

// The pipeline assigns the id before it tells the strategy the order exists, and reports carry it back. The
// tests have to do the same, because a strategy that keyed its working child on 0 would mistake "no child" for
// "the child it just sent".
StrategyOrder identified(StrategyOrder order, uint64_t id)
{
    order.order_id = id;
    return order;
}

// Deliver what the pipeline would: the order is submitted, its fill is reported, and the terminal report that
// closes it follows. Without the terminal report the child would still look like it is working.
void fill(ExecutionL2Strategy& strategy, const StrategyOrder& order)
{
    strategy.on_order_submitted(order);
    ExecutionReport report;
    report.order_id = order.order_id;
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
        fill(strategy, identified(first[0], 1));
        // Nothing left to do: the parent is complete.
        assert(strategy.on_l2_market_view(view_at(2'000'000, 100.0)).empty());
        assert(strategy.filled_quantity() == 100);
    }

    // Twap: ten children of ten across a ten-second horizon, and none after the deadline.
    {
        ExecutionL2Strategy strategy(parent(ExecutionAlgo::Twap));
        uint64_t children = 0;
        uint64_t total = 0;
        uint64_t next_id = 0;
        for (uint64_t second = 1; second <= 14; ++second)
        {
            for (const auto& order : strategy.on_l2_market_view(view_at(second * 1'000'000, 100.0)))
            {
                ++children;
                assert(order.quantity == 10);
                total += order.quantity;
                fill(strategy, identified(order, ++next_id));
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

    // A crossing child that is still open after the view it was sent on is pulled on the next view, so a venue
    // or an override that ignores the time in force cannot leave two children working at once.
    {
        ExecutionL2Strategy strategy(parent(ExecutionAlgo::Market));
        const auto first = strategy.on_l2_market_view(view_at(1'000'000, 100.0));
        assert(first.size() == 1);
        const auto sent = identified(first[0], 7);
        strategy.on_order_submitted(sent);
        assert(strategy.cancel_requests().empty());
        strategy.on_l2_market_view(view_at(2'000'000, 100.0));
        const auto stale = strategy.cancel_requests();
        assert(stale.size() == 1);
        assert(stale[0] == sent.order_id);
    }

    // Post-first: the child joins the near touch and waits. When its slice's timeout passes it is pulled, and
    // what the slice still owes is crossed on the same view rather than waiting for the next slice.
    {
        auto config = parent(ExecutionAlgo::Market);
        config.post_first = true;
        config.post_timeout_us = 5'000'000;
        ExecutionL2Strategy strategy(config);
        const auto posted = strategy.on_l2_market_view(view_at(1'000'000, 100.0));
        assert(posted.size() == 1);
        assert(posted[0].price == 99.5);
        assert(posted[0].time_in_force == TimeInForce::Gtc);
        const auto sent_post = identified(posted[0], 11);
        strategy.on_order_submitted(sent_post);

        // Three seconds in, the child is still waiting: nothing new is sent.
        assert(strategy.on_l2_market_view(view_at(4'000'000, 100.0)).empty());
        assert(strategy.posted_children() == 1);

        // Past the timeout the child is pulled and the remainder crosses in the same view.
        const auto fallback = strategy.on_l2_market_view(view_at(7'000'000, 100.0));
        assert(fallback.size() == 1);
        assert(fallback[0].price == 100.5);
        assert(fallback[0].time_in_force == TimeInForce::Ioc);
        assert(fallback[0].quantity == 100);
        const auto pulled = strategy.cancel_requests();
        assert(pulled.size() == 1);
        assert(pulled[0] == sent_post.order_id);
        assert(strategy.crossed_children() == 0);
    }

    // A posted child that fills part of its slice hands only the rest to the crossing fallback.
    {
        auto config = parent(ExecutionAlgo::Market);
        config.post_first = true;
        config.post_timeout_us = 5'000'000;
        ExecutionL2Strategy strategy(config);
        const auto posted = strategy.on_l2_market_view(view_at(1'000'000, 100.0));
        assert(posted.size() == 1);
        const auto sent_post = identified(posted[0], 21);
        strategy.on_order_submitted(sent_post);
        ExecutionReport partial;
        partial.order_id = sent_post.order_id;
        partial.exec_type = ExecType::Trade;
        partial.side = exchange::Side::Buy;
        partial.price = sent_post.price;
        partial.quantity = 40;
        strategy.on_order_update(partial);
        assert(strategy.filled_quantity() == 40);

        const auto fallback = strategy.on_l2_market_view(view_at(7'000'000, 100.0));
        assert(fallback.size() == 1);
        assert(fallback[0].quantity == 60);
    }

    // Without a parent the strategy is inert rather than guessing one.
    {
        ExecutionL2Strategy strategy(ExecutionL2Config{});
        assert(strategy.on_l2_market_view(view_at(1'000'000, 100.0)).empty());
    }
}
