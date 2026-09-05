#include <cassert>
#include <cstdint>

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