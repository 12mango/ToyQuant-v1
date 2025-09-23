#pragma once
#include "../market/csv_feed.h"
#include "../orderbook/orderbook.h"
#include "../strategy/market_maker.h"
#include "../exchange/matching_engine.h"
#include <atomic>
#include <fstream>
#include <string>

class BacktestDriver {
public:
    BacktestDriver(const std::string& tick_file,
                   const std::string& orders_file,
                   const std::string& trades_file);

    void run();

private:
    OrderBook ob;
    MarketMaker strat;
    MatchingEngine engine;

    std::atomic<uint64_t> next_order_id{1};
    std::ofstream orders_out;
    std::ofstream trades_out;

    void on_tick(const Tick& t);
    void on_report(const ExecutionReport& report);
};
