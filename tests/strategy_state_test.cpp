#include <cassert>

#include "strategy/market_maker.h"

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
}