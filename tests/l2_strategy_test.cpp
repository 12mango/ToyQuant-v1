#include <cassert>

#include "orderbook/l2_orderbook.h"
#include "strategy/l2_market_maker.h"

int main()
{
    L2OrderBook book;
    book.apply_snapshot(MarketDepthSnapshot{.ts = 100,
                                            .symbol = "BTC-PERPETUAL",
                                            .sequence = 1,
                                            .bids = {{6421.0, 141360}, {6420.5, 100}},
                                            .asks = {{6421.5, 18640}, {6422.0, 200}}});

    const L2MarketView view = book.market_view();
    assert(view.top.bid_price == 6421.0);
    assert(view.top.ask_price == 6421.5);
    assert(view.depth_imbalance > 0.0);
    assert(view.micro_price > view.top.bid_price);

    L2MarketMaker strategy(L2MarketMakerConfig{.order_size = 1,
                                                .base_spread = 1.0,
                                                .inventory_limit = 100,
                                                .tick_size = 0.5,
                                                .imbalance_shift = 1.0});
    const auto orders = strategy.on_l2_market_view(view);
    assert(orders.size() == 2);
    assert(orders[0].side == Side::Buy);
    assert(orders[1].side == Side::Sell);
    assert(orders[0].price < orders[1].price);

    L2MarketMaker baseline(L2MarketMakerConfig{.order_size = 1,
                                                .base_spread = 1.0,
                                                .inventory_limit = 100,
                                                .tick_size = 0.5,
                                                .signal_mode = L2SignalMode::Baseline});
    const auto baseline_orders = baseline.on_l2_market_view(view);
    assert(baseline_orders.size() == 2);
    assert(baseline_orders[0].price < orders[0].price);

    L2MarketMaker refreshable(L2MarketMakerConfig{.order_size = 1,
                                                   .base_spread = 1.0,
                                                   .inventory_limit = 100,
                                                   .tick_size = 0.5,
                                                   .refresh_price_ticks = 2,
                                                   .max_quote_age = 10});
    const auto initial_orders = refreshable.on_l2_market_view(view);
    assert(initial_orders.size() == 2);
    refreshable.on_order_submitted(StrategyOrder{Side::Buy, view.symbol, initial_orders[0].price, 1,
                                                 1});
    refreshable.on_order_submitted(StrategyOrder{Side::Sell, view.symbol, initial_orders[1].price, 1,
                                                 2});
    assert(refreshable.on_l2_market_view(view).empty());
    assert(refreshable.cancel_requests().empty());

    L2MarketView moved_view = view;
    moved_view.top.bid_price += 2.0;
    moved_view.top.ask_price += 2.0;
    assert(refreshable.on_l2_market_view(moved_view).empty());
    assert(refreshable.cancel_requests().size() == 2);
}
