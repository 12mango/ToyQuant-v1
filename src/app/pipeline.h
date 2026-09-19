#pragma once

#include <atomic>
#include <fstream>
#include <string>
#include <unordered_map>

#include "app/run_summary.h"
#include "exchange/matching_engine.h"
#include "market/market_event.h"
#include "orderbook/orderbook.h"
#include "strategy/strategy.h"

class Logger;

class Pipeline
{
   public:
    Pipeline(std::ofstream& orders_out, std::ofstream& trades_out, IOrderBook& order_book,
             Strategy& strategy, IMatchingEngine& engine, Portfolio& portfolio, Logger& logger);

    void process_event(const MarketEvent& event, bool enable_print = false);
    void process_top_of_book(const std::string& symbol, uint64_t ts, const TopOfBook& top,
                             bool enable_print = false);
    RunSummary summary() const;

   private:
    void submit_strategy_actions(const std::string& symbol, uint64_t ts, const TopOfBook& top);

    std::ofstream& orders_out_;
    std::ofstream& trades_out_;
    IOrderBook& order_book_;
    Strategy& strategy_;
    IMatchingEngine& engine_;
    Portfolio& portfolio_;
    Logger& logger_;
    std::unordered_map<std::string, BboQuote> latest_quotes_;
    std::atomic<uint64_t> next_order_id_{1};
    uint64_t submitted_orders_{0};
    uint64_t submitted_quantity_{0};
    uint64_t cancel_requests_{0};
    uint64_t trade_reports_{0};
    uint64_t trade_report_quantity_{0};
};
