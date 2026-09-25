#include <cassert>

#include "orderbook/l2_orderbook.h"
#include "strategy/active_l2_market_maker.h"
#include "strategy/inventory_aware_l2_market_maker.h"
#include "strategy/l2_market_maker.h"

int main()
{
    L2OrderBook book;
    book.apply_snapshot(MarketDepthSnapshot{.ts = 100,
                                            .symbol = "BTC-PERPETUAL",
                                            .sequence = 1,
                                            .bids = {{6421.0, 141360}, {6420.5, 100}},
                                            .asks = {{6421.5, 18640}, {6422.0, 200}},
                                            .exchange = "deribit"});

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

    L2MarketMaker passive_guard(L2MarketMakerConfig{.order_size = 1,
                                                     .base_spread = 0.5,
                                                     .inventory_limit = 100,
                                                     .tick_size = 0.5,
                                                     .imbalance_shift = 10.0,
                                                     .signal_mode = L2SignalMode::Depth});
    const auto guarded_orders = passive_guard.on_l2_market_view(view);
    assert(guarded_orders.size() == 2);
    assert(guarded_orders[0].price <= view.top.bid_price);
    assert(guarded_orders[1].price >= view.top.ask_price);

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

    L2MarketMaker guarded_buy_flow(L2MarketMakerConfig{.order_size = 1,
                                                        .base_spread = 1.0,
                                                        .inventory_limit = 100,
                                                        .tick_size = 0.5,
                                                        .signal_mode = L2SignalMode::Flow,
                                                        .toxicity_flow_threshold = 0.5});
    guarded_buy_flow.on_market_trade(MarketTrade{99, view.symbol, 6421.5, 100, Side::Buy, 1,
                                                 "deribit"});
    const auto buy_flow_orders = guarded_buy_flow.on_l2_market_view(view);
    assert(buy_flow_orders.size() == 1);
    assert(buy_flow_orders[0].side == Side::Buy);

    L2MarketMaker guarded_sell_flow(L2MarketMakerConfig{.order_size = 1,
                                                         .base_spread = 1.0,
                                                         .inventory_limit = 100,
                                                         .tick_size = 0.5,
                                                         .signal_mode = L2SignalMode::Flow,
                                                         .toxicity_flow_threshold = 0.5});
    guarded_sell_flow.on_market_trade(MarketTrade{99, view.symbol, 6421.0, 100, Side::Sell, 1,
                                                  "deribit"});
    const auto sell_flow_orders = guarded_sell_flow.on_l2_market_view(view);
    assert(sell_flow_orders.size() == 1);
    assert(sell_flow_orders[0].side == Side::Sell);

    L2MarketMaker short_flow(L2MarketMakerConfig{.order_size = 1,
                                                  .base_spread = 1.0,
                                                  .inventory_limit = 100,
                                                  .tick_size = 0.5,
                                                  .signal_mode = L2SignalMode::Flow,
                                                  .trade_window = 1,
                                                  .toxicity_flow_threshold = 0.5});
    short_flow.on_market_trade(MarketTrade{99, view.symbol, 6421.5, 100, Side::Buy, 1,
                                           "deribit"});
    short_flow.on_market_trade(MarketTrade{100, view.symbol, 6421.0, 100, Side::Sell, 2,
                                           "deribit"});
    const auto latest_flow_orders = short_flow.on_l2_market_view(view);
    assert(latest_flow_orders.size() == 1);
    assert(latest_flow_orders[0].side == Side::Sell);

    InventoryAwareL2MarketMaker inventory_strategy(
        InventoryAwareL2MarketMakerConfig{
            .base = L2MarketMakerConfig{.order_size = 2,
                                         .base_spread = 1.0,
                                         .inventory_limit = 2,
                                         .tick_size = 0.5,
                                         .signal_mode = L2SignalMode::Flow,
                                         .toxicity_flow_threshold = 0.65},
            .inventory_limit = 2,
            .max_inventory_shift_ticks = 2.0});
    const auto initial_inventory_orders = inventory_strategy.on_l2_market_view(view);
    assert(initial_inventory_orders.size() == 2);
    inventory_strategy.on_order_submitted(
        StrategyOrder{Side::Buy, view.symbol, initial_inventory_orders[0].price, 2, 10});
    inventory_strategy.on_order_update(ExecutionReport{.order_id = 10,
                                                        .side = exchange::Side::Buy,
                                                        .exec_type = ExecType::Trade,
                                                        .symbol = view.symbol,
                                                        .price = initial_inventory_orders[0].price,
                                                        .quantity = 2,
                                                        .owner = "MarketMaker"});
    const auto long_inventory_orders = inventory_strategy.on_l2_market_view(view);
    assert(long_inventory_orders.size() == 1);
    assert(long_inventory_orders[0].side == Side::Sell);

    ActiveL2MarketMaker active(ActiveL2MarketMakerConfig{
        .base = L2MarketMakerConfig{.order_size = 2,
                                     .base_spread = 1.0,
                                     .inventory_limit = 100,
                                     .tick_size = 0.5,
                                     .signal_mode = L2SignalMode::Flow,
                                     .toxicity_flow_threshold = 0.65},
        .volatility_alpha = 0.5,
        .pause_after_ticks = 8.0});
    assert(active.on_l2_market_view(view).size() == 2);
    L2MarketView volatile_view = view;
    assert(active.on_l2_market_view(view).size() == 2);
    bool paused = false;
    for (int index = 0; index < 10; ++index)
    {
        volatile_view.top.bid_price += index % 2 == 0 ? 10.0 : -10.0;
        volatile_view.top.ask_price += index % 2 == 0 ? 10.0 : -10.0;
        if (active.on_l2_market_view(volatile_view).empty()) paused = true;
    }
    assert(paused);
    assert(active.volatility_ticks() >= 8.0);

    ActiveL2MarketMaker time_aware(ActiveL2MarketMakerConfig{
        .base = L2MarketMakerConfig{.order_size = 2,
                                     .base_spread = 1.0,
                                     .inventory_limit = 100,
                                     .tick_size = 0.5,
                                     .signal_mode = L2SignalMode::Flow,
                                     .toxicity_flow_threshold = 0.65},
        .volatility_alpha = 0.5,
                        .pause_after_ticks = 8.0});
    L2MarketView base_time_view = view;
    base_time_view.ts = 1000;
    time_aware.on_l2_market_view(base_time_view);
    L2MarketView slow_move = base_time_view;
    slow_move.ts = 2000000;
    slow_move.top.bid_price += 10.0;
    slow_move.top.ask_price += 10.0;
    time_aware.on_l2_market_view(slow_move);
    const double slow_volatility = time_aware.volatility_ticks();

    ActiveL2MarketMaker fast_time_aware(ActiveL2MarketMakerConfig{
        .base = L2MarketMakerConfig{.order_size = 2,
                                     .base_spread = 1.0,
                                     .inventory_limit = 100,
                                     .tick_size = 0.5,
                                     .signal_mode = L2SignalMode::Flow,
                                     .toxicity_flow_threshold = 0.65},
        .volatility_alpha = 0.5,
                        .pause_after_ticks = 8.0});
    fast_time_aware.on_l2_market_view(base_time_view);
    L2MarketView fast_move = base_time_view;
    fast_move.ts = 1001;
    fast_move.top.bid_price += 10.0;
    fast_move.top.ask_price += 10.0;
    fast_time_aware.on_l2_market_view(fast_move);
    const double fast_volatility = fast_time_aware.volatility_ticks();
    assert(slow_volatility > 0.0);
    assert(fast_volatility > slow_volatility);

    ActiveL2MarketMaker pausing(ActiveL2MarketMakerConfig{
        .base = L2MarketMakerConfig{.order_size = 2,
                                     .base_spread = 1.0,
                                     .inventory_limit = 100,
                                     .tick_size = 0.5,
                                     .signal_mode = L2SignalMode::Flow,
                                     .toxicity_flow_threshold = 0.65},
        .volatility_alpha = 1.0,
                        .pause_after_ticks = 5.0});
    L2MarketView calm_view = view;
    calm_view.ts = 1000000;
    const auto pausing_orders = pausing.on_l2_market_view(calm_view);
    assert(pausing_orders.size() == 2);
    pausing.on_order_submitted(
        StrategyOrder{Side::Buy, view.symbol, pausing_orders[0].price, 2, 21});
    pausing.on_order_submitted(
        StrategyOrder{Side::Sell, view.symbol, pausing_orders[1].price, 2, 22});
    L2MarketView shock_view = calm_view;
    shock_view.ts += 1000000;
    shock_view.top.bid_price += 5.0;
    shock_view.top.ask_price += 5.0;
    assert(pausing.on_l2_market_view(shock_view).empty());
    assert(pausing.cancel_requests().size() == 2);
}
