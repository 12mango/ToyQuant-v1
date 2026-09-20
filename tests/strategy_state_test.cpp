#include <array>
#include <cassert>
#include <cstdint>

#include "app/strategy_factory.h"
#include "strategy/market_maker.h"

uint64_t quantity_for_side(const std::vector<StrategyOrder>& orders, Side side)
{
    uint64_t total = 0;
    for (const auto& order : orders)
    {
        if (order.side == side) total += order.quantity;
    }
    return total;
}

int main()
{
    const auto instrument = btc_usdt_spec(100);
    auto passive_l1 = make_strategy("passive_l1", &instrument);
    assert(passive_l1 != nullptr);
    assert(dynamic_cast<PassiveL1MarketMaker*>(passive_l1.get()) != nullptr);

    auto inventory_aware_l1 = make_strategy("inventory_aware_l1", &instrument);
    assert(inventory_aware_l1 != nullptr);
    assert(dynamic_cast<InventoryAwareL1MarketMaker*>(inventory_aware_l1.get()) != nullptr);

    auto flow_aware_l1 = make_strategy("flow_aware_l1", &instrument);
    assert(flow_aware_l1 != nullptr);
    assert(dynamic_cast<FlowAwareL1MarketMaker*>(flow_aware_l1.get()) != nullptr);

    InventoryAwareL1MarketMaker long_inventory_l1(100, 0.00003,
                                                 static_cast<int64_t>(instrument.quantity_scale / 10),
                                                 instrument.tick_size);
    long_inventory_l1.position = 500;
    const TopOfBook inventory_top{100.0, 100, 100.1, 100};
    const auto inventory_orders = long_inventory_l1.on_top_of_book("BTCUSDT", inventory_top);
    assert(quantity_for_side(inventory_orders, Side::Buy) <
           quantity_for_side(inventory_orders, Side::Sell));

    FlowAwareL1MarketMaker neutral_flow_strategy(100, 2.0 * instrument.tick_size,
                                                 static_cast<int64_t>(instrument.quantity_scale / 10),
                                                 instrument.tick_size);
    const TopOfBook flow_top{100.0, 100, 100.1, 100};
    const auto neutral_flow_orders =
        neutral_flow_strategy.on_top_of_book("BTCUSDT", flow_top);

    FlowAwareL1MarketMaker buy_flow_strategy(100, 2.0 * instrument.tick_size,
                                            static_cast<int64_t>(instrument.quantity_scale / 10),
                                            instrument.tick_size);
    buy_flow_strategy.on_market_trade(MarketTrade{1, "BTCUSDT", 100.0, 100, Side::Buy, 10});
    buy_flow_strategy.on_market_trade(MarketTrade{2, "BTCUSDT", 100.0, 100, Side::Buy, 10});
    const auto flow_orders = buy_flow_strategy.on_top_of_book("BTCUSDT", flow_top);
    assert(neutral_flow_orders.size() == 2);
    assert(flow_orders.size() == 2);
    assert(flow_orders[0].price == neutral_flow_orders[0].price);
    assert(flow_orders[1].price >= neutral_flow_orders[1].price + instrument.tick_size);
    assert(quantity_for_side(flow_orders, Side::Buy) == 100);
    assert(quantity_for_side(flow_orders, Side::Sell) == 80);

    L1MarketMaker l1_strategy(100, 0.0002, 1000, 0.0001);
    const TopOfBook l1_top{100.0, 100, 100.1, 100};
    auto l1_orders = l1_strategy.on_top_of_book("BTCUSDT", l1_top);
    assert(quantity_for_side(l1_orders, Side::Buy) == 100);
    assert(quantity_for_side(l1_orders, Side::Sell) == 100);
    l1_orders[0].order_id = 1;
    l1_strategy.on_order_submitted(l1_orders[0]);
    assert(l1_strategy.on_top_of_book("BTCUSDT", l1_top).empty());
    assert(l1_strategy.cancel_requests().empty());
    l1_strategy.on_top_of_book("BTCUSDT", TopOfBook{100.1, 100, 100.2, 100});
    assert(l1_strategy.cancel_requests().size() == 1);
    l1_strategy.on_order_update(ExecutionReport{1, exchange::Side::Buy, ExecType::Cancelled,
                                                "BTCUSDT", 99.9, 100, 2, "MarketMaker"});
    l1_orders = l1_strategy.on_top_of_book("BTCUSDT", TopOfBook{100.1, 100, 100.2, 100});
    assert(quantity_for_side(l1_orders, Side::Buy) == 100);
    assert(quantity_for_side(l1_orders, Side::Sell) == 100);
    l1_strategy.position = 1000;
    l1_orders = l1_strategy.on_top_of_book("BTCUSDT", l1_top);
    assert(quantity_for_side(l1_orders, Side::Buy) == 0);
    assert(quantity_for_side(l1_orders, Side::Sell) == 100);
    l1_orders[0].order_id = 2;
    l1_strategy.on_order_submitted(l1_orders[0]);
    l1_strategy.on_order_update(ExecutionReport{0, exchange::Side::Buy, ExecType::Trade, "BTCUSDT",
                                                100.1, 500, 2, "Market"});
    assert(l1_strategy.position == 1000);
    l1_strategy.on_top_of_book("BTCUSDT", TopOfBook{100.0, 100, 0.0, 0});
    assert(l1_strategy.cancel_requests().size() == 1);

    L1MarketMaker neutral_strategy(100, 0.2, 1000, 0.1);
    L1MarketMaker buy_pressure_strategy(100, 0.2, 1000, 0.1);
    const TopOfBook pressure_top{100.0, 100, 100.1, 100};
    const auto neutral_orders = neutral_strategy.on_top_of_book("BTCUSDT", pressure_top);
    buy_pressure_strategy.on_market_trade(MarketTrade{1, "BTCUSDT", 100.2, 100, Side::Buy, 1});
    buy_pressure_strategy.on_market_trade(MarketTrade{2, "BTCUSDT", 100.2, 100, Side::Buy, 2});
    const auto pressure_orders = buy_pressure_strategy.on_top_of_book("BTCUSDT", pressure_top);
    assert(pressure_orders.size() == 2);
    assert(pressure_orders[0].price < neutral_orders[0].price);
    assert(pressure_orders[1].price > neutral_orders[1].price);

    L1MarketMaker dynamic_strategy(100, 0.2, 1000, 0.1);
    const auto tight_orders =
        dynamic_strategy.on_top_of_book("BTCUSDT", TopOfBook{100.0, 100, 100.1, 100});
    dynamic_strategy.on_order_submitted(
        StrategyOrder{Side::Buy, "BTCUSDT", tight_orders[0].price, tight_orders[0].quantity, 10});
    const auto wide_orders =
        dynamic_strategy.on_top_of_book("BTCUSDT", TopOfBook{99.0, 100, 101.0, 100});
    assert(wide_orders.empty());
    assert(dynamic_strategy.cancel_requests().size() == 1);

    L1MarketMaker depth_strategy(100, 0.4, 1000, 0.1);
    const auto balanced_orders =
        depth_strategy.on_top_of_book("BTCUSDT", TopOfBook{99.0, 100, 101.0, 100});
    L1MarketMaker bid_heavy_strategy(100, 0.4, 1000, 0.1);
    const auto bid_heavy_orders =
        bid_heavy_strategy.on_top_of_book("BTCUSDT", TopOfBook{99.0, 1000, 101.0, 100});
    assert(bid_heavy_orders[0].price < balanced_orders[0].price);
    assert(bid_heavy_orders[1].price > balanced_orders[1].price);

    L1MarketMaker time_ordered_imbalance(100, 0.2, 1000, 0.1);
    time_ordered_imbalance.trade_imbalance_window = 4;
    const std::array<MarketTrade, 6> mixed_flow = {
        MarketTrade{1, "BTCUSDT", 100.0, 100, Side::Buy, 4},
        MarketTrade{2, "BTCUSDT", 100.0, 100, Side::Buy, 4},
        MarketTrade{3, "BTCUSDT", 100.0, 100, Side::Sell, 1},
        MarketTrade{4, "BTCUSDT", 100.0, 100, Side::Sell, 1},
        MarketTrade{5, "BTCUSDT", 100.0, 100, Side::Sell, 1},
        MarketTrade{6, "BTCUSDT", 100.0, 100, Side::Buy, 4},
    };
    for (const auto& trade : mixed_flow) time_ordered_imbalance.on_market_trade(trade);
    const auto time_ordered_orders =
        time_ordered_imbalance.on_top_of_book("BTCUSDT", TopOfBook{100.0, 100, 100.1, 100});
    assert(time_ordered_orders.size() == 2);
    assert(time_ordered_orders[0].price < 100.0);
    assert(time_ordered_orders[1].price > 100.1);

    L1MarketMaker stale_trade_strategy(100, 0.2, 1000, 0.1);
    const BboQuote stale_reference{1000, "BTCUSDT", 99.9, 100, 100.1, 100, 1};
    stale_trade_strategy.on_market_trade(MarketTrade{2001, "BTCUSDT", 100.0, 100, Side::Buy, 1},
                                         &stale_reference);
    assert(stale_trade_strategy.buy_volume == 0);
    stale_trade_strategy.on_market_trade(MarketTrade{1001, "BTCUSDT", 110.0, 100, Side::Buy, 2},
                                         &stale_reference);
    assert(stale_trade_strategy.buy_volume == 0);

    L1MarketMaker metrics_strategy(10, 0.2, 1000, 0.1);
    auto metric_orders =
        metrics_strategy.on_top_of_book("BTCUSDT", TopOfBook{100.0, 100, 100.1, 100});
    metric_orders[0].order_id = 11;
    metrics_strategy.on_order_submitted(metric_orders[0]);
    metrics_strategy.on_order_update(ExecutionReport{11, exchange::Side::Buy, ExecType::Trade,
                                                     "BTCUSDT", 100.0, 5, 1, "MarketMaker"});
    for (int i = 0; i < 5; ++i)
        metrics_strategy.on_top_of_book("BTCUSDT", TopOfBook{99.8, 100, 99.9, 100});
    metrics_strategy.on_order_update(ExecutionReport{11, exchange::Side::Buy, ExecType::Cancelled,
                                                     "BTCUSDT", 100.0, 5, 1, "MarketMaker"});
    assert(metrics_strategy.markout_count == 1);
    assert(metrics_strategy.adverse_selection > 0.0);
    assert(metrics_strategy.inventory_samples >= 6);
    assert(metrics_strategy.max_abs_inventory >= 10);
    assert(metrics_strategy.total_quote_lifetime == 5);
    const auto metrics = metrics_strategy.metrics();
    assert(metrics.filled_quantity == 5);
    assert(metrics.markout_count == 1);

    OptimizedMarketMaker strategy;

    const StrategyOrder buy{Side::Buy, "EURUSD", 1.10000, 100, 1};
    strategy.on_order_submitted(buy);
    strategy.on_order_update(ExecutionReport{1, exchange::Side::Buy, ExecType::Trade, "EURUSD",
                                             1.10000, 40, 1, "MarketMaker"});
    assert(strategy.position == 40);
    assert(strategy.open_orders.at(1).quantity == 60);

    strategy.on_order_update(ExecutionReport{1, exchange::Side::Buy, ExecType::PartialFill,
                                             "EURUSD", 1.10000, 60, 1, "MarketMaker"});
    assert(strategy.position == 40);
    assert(strategy.open_orders.at(1).quantity == 60);

    strategy.on_order_update(ExecutionReport{1, exchange::Side::Buy, ExecType::Trade, "EURUSD",
                                             1.10000, 60, 2, "MarketMaker"});
    strategy.on_order_update(ExecutionReport{1, exchange::Side::Buy, ExecType::Filled, "EURUSD",
                                             1.10000, 0, 2, "MarketMaker"});
    assert(strategy.position == 100);
    assert(strategy.open_orders.empty());

    const StrategyOrder sell{Side::Sell, "EURUSD", 1.10010, 50, 2};
    strategy.on_order_submitted(sell);
    strategy.on_order_update(ExecutionReport{2, exchange::Side::Sell, ExecType::Trade, "EURUSD",
                                             1.10010, 50, 3, "MarketMaker"});
    strategy.on_order_update(ExecutionReport{2, exchange::Side::Sell, ExecType::Filled, "EURUSD",
                                             1.10010, 0, 3, "MarketMaker"});
    assert(strategy.position == 50);
    assert(strategy.open_orders.empty());

    OptimizedMarketMaker inventory_strategy;
    const TopOfBook top{1.10000, 100, 1.10020, 100};

    inventory_strategy.position = 500;
    auto long_orders = inventory_strategy.on_top_of_book("EURUSD", top);
    assert(quantity_for_side(long_orders, Side::Sell) > quantity_for_side(long_orders, Side::Buy));

    inventory_strategy.position = -500;
    auto short_orders = inventory_strategy.on_top_of_book("EURUSD", top);
    assert(quantity_for_side(short_orders, Side::Buy) >
           quantity_for_side(short_orders, Side::Sell));

    inventory_strategy.position = 1200;
    auto overlong_orders = inventory_strategy.on_top_of_book("EURUSD", top);
    assert(quantity_for_side(overlong_orders, Side::Buy) == 0);
    assert(quantity_for_side(overlong_orders, Side::Sell) > 0);
}