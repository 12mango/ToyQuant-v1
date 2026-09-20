#include <cassert>
#include <cmath>
#include <fstream>

#include "accounting/portfolio.h"
#include "app/pipeline.h"
#include "exchange/matching_engine.h"
#include "orderbook/orderbook.h"
#include "strategy/market_maker.h"
#include "utils/logger.h"

int main()
{
    std::ofstream orders("/tmp/pipeline_accounting_orders.csv");
    std::ofstream trades("/tmp/pipeline_accounting_trades.csv");
    NaiveMarketMaker strategy(10, 2.0, 1.0);
    OrderBook order_book(1.0);
    MatchingEngine engine(nullptr, 1.0,
                          FeeSchedule{.maker_rate = 0.001,
                                      .taker_rate = 0.002,
                                      .quantity_scale = 1000});
    Portfolio portfolio(1000, 1000.0);
    Logger logger;
    Pipeline pipeline(orders, trades, order_book, strategy, engine, portfolio, logger);

    pipeline.process_event(BboQuote{1, "TEST", 99.0, 100, 101.0, 100, 1});
    pipeline.process_event(MarketTrade{2, "TEST", 99.0, 10, Side::Sell, 2});

    const auto* position = portfolio.find_position("TEST");
    const auto metrics = portfolio.metrics();
    assert(position != nullptr);
    assert(position->quantity == 10);
    assert(metrics.maker_trade_count == 1);
    assert(metrics.taker_trade_count == 0);
    assert(std::abs(metrics.fees_paid - 0.00099) < 1e-12);
    assert(pipeline.summary().trade_reports == 1);
}