// The non-market-making strategy has one contract worth checking: a signal sets a target position, the
// target is reached in clips at the touch, and the two risk exits (a stop and a time stop) take priority
// over a signal that is still pointing the other way. The test drives the strategy directly; the pipeline
// assigns order ids in a run, so the harness does the same here.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

#include "strategy/trend_l2.h"

namespace
{
L2MarketView view_at(double mid)
{
    L2MarketView view;
    view.symbol = "TEST";
    view.top = TopOfBook{mid - 0.01, 100, mid + 0.01, 100};
    view.micro_price = mid;
    return view;
}

struct Harness
{
    explicit Harness(TrendL2Config config) : strategy(config) {}

    std::vector<StrategyOrder> on(double mid)
    {
        auto orders = strategy.on_l2_market_view(view_at(mid));
        for (StrategyOrder& order : orders)
        {
            order.order_id = next_id++;
            strategy.on_order_submitted(order);
        }
        return orders;
    }

    void fill(const StrategyOrder& order, uint64_t quantity)
    {
        strategy.on_order_update(ExecutionReport{
            .order_id = order.order_id,
            .side = order.side == Side::Buy ? exchange::Side::Buy : exchange::Side::Sell,
            .exec_type = ExecType::Trade,
            .symbol = order.symbol,
            .price = order.price,
            .quantity = quantity,
            .ts = 0,
            .owner = "MarketMaker"});
    }

    TrendL2Strategy strategy;
    uint64_t next_id{1};
};
}  // namespace

int main()
{
    {
        // A rising mid: warm up, then three clips take the position to the target, and a falling mid
        // through the exit band sends the first reducing clip.
        Harness h(TrendL2Config{.order_size = 10,
                                .max_position = 30,
                                .tick_size = 0.01,
                                .fast_views = 2,
                                .slow_views = 4,
                                .entry_ticks = 1.0,
                                .exit_ticks = 0.2,
                                .stop_ticks = 5.0,
                                .max_holding_views = 100});

        for (int index = 0; index < 4; ++index) assert(h.on(100.00).empty());

        auto orders = h.on(100.05);
        assert(orders.size() == 1);
        assert(orders[0].side == Side::Buy);
        assert(orders[0].quantity == 10);
        assert(std::abs(orders[0].price - 100.06) < 1e-9);
        h.fill(orders[0], 10);
        assert(h.strategy.net_position() == 10);

        orders = h.on(100.10);
        assert(orders.size() == 1);
        h.fill(orders[0], 10);
        orders = h.on(100.15);
        assert(orders.size() == 1);
        h.fill(orders[0], 10);
        assert(h.strategy.net_position() == 30);
        assert(h.strategy.target_position() == 30);

        // At the target there is nothing to do.
        assert(h.on(100.20).empty());

        // The gap falls inside the exit band: the target is flat and the first reducing clip goes out.
        orders = h.on(100.00);
        assert(orders.size() == 1);
        assert(orders[0].side == Side::Sell);
        assert(orders[0].quantity == 10);
        h.fill(orders[0], 10);
        assert(h.strategy.net_position() == 20);
    }
    {
        // A stop takes priority over the signal: a long entered at 100.11 is exited when the mid reaches
        // 100.04, five ticks against it, and the risk pause is counted.
        Harness h(TrendL2Config{.order_size = 10,
                                .max_position = 10,
                                .tick_size = 0.01,
                                .fast_views = 2,
                                .slow_views = 4,
                                .entry_ticks = 1.0,
                                .exit_ticks = 0.2,
                                .stop_ticks = 5.0,
                                .max_holding_views = 100,
                                .exit_cooldown_views = 3});

        for (int index = 0; index < 4; ++index) h.on(100.00);
        auto orders = h.on(100.10);
        assert(orders.size() == 1);
        h.fill(orders[0], 10);
        assert(h.strategy.net_position() == 10);

        orders = h.on(100.04);
        assert(orders.size() == 1);
        assert(orders[0].side == Side::Sell);
        assert(h.strategy.metrics().risk_pause_count >= 1);

        // The exit fills, and the cooldown then refuses to reopen the same position for three views
        // even though the signal is rising again.
        h.fill(orders[0], 10);
        assert(h.strategy.net_position() == 0);
        for (int index = 0; index < 3; ++index) assert(h.on(100.30).empty());
        assert(!h.on(100.30).empty());
    }
    {
        // A refused entry is not retried on the next view: the wait starts at two views, doubles on each
        // consecutive refusal, and a successful submission resets it.
        Harness h(TrendL2Config{.order_size = 10,
                                .max_position = 10,
                                .tick_size = 0.01,
                                .fast_views = 2,
                                .slow_views = 4,
                                .entry_ticks = 1.0,
                                .exit_ticks = 0.2,
                                .stop_ticks = 5.0,
                                .max_holding_views = 100,
                                .exit_cooldown_views = 0,
                                .rejection_backoff_views = 2,
                                .max_rejection_backoff_views = 8});
        for (int index = 0; index < 4; ++index) h.on(100.00);

        // The pipeline would refuse this order instead of submitting it, so it is not in open_orders.
        auto orders = h.strategy.on_l2_market_view(view_at(100.10));
        assert(orders.size() == 1);
        h.strategy.on_order_rejected(orders[0], "rate_limit");
        h.strategy.on_order_rejected(orders[0], "rate_limit");
        // The rejection was on view 5 and the doubled wait is four views: 6, 7 and 8 are blocked, 9 retries.
        assert(h.on(100.15).empty());
        assert(h.on(100.20).empty());
        assert(h.on(100.25).empty());
        assert(!h.on(100.30).empty());
    }

    {
        // A venue refusal arrives as a cancelled report plus the rejection callback: the order leaves the open
        // set, and the next attempt waits rather than resubmitting on the next view.
        Harness h(TrendL2Config{.order_size = 10,
                                .max_position = 10,
                                .tick_size = 0.01,
                                .fast_views = 2,
                                .slow_views = 4,
                                .entry_ticks = 1.0,
                                .exit_ticks = 0.2,
                                .stop_ticks = 5.0,
                                .max_holding_views = 100,
                                .exit_cooldown_views = 0,
                                .rejection_backoff_views = 2,
                                .max_rejection_backoff_views = 8});
        for (int index = 0; index < 4; ++index) h.on(100.00);
        auto orders = h.on(100.10);
        assert(orders.size() == 1);
        assert(h.strategy.working_order_count() == 1);

        h.strategy.on_order_rejected(orders[0], "venue_reject");
        h.strategy.on_order_update(ExecutionReport{.order_id = orders[0].order_id,
                                                   .side = exchange::Side::Buy,
                                                   .exec_type = ExecType::Cancelled,
                                                   .symbol = "TEST",
                                                   .price = orders[0].price,
                                                   .quantity = orders[0].quantity,
                                                   .ts = 0,
                                                   .owner = "MarketMaker"});
        assert(h.strategy.working_order_count() == 0);
        assert(h.on(100.15).empty());
        assert(!h.on(100.20).empty());
    }

    return 0;
}
