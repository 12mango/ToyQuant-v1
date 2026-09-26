#pragma once

#include <atomic>
#include <deque>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
             Strategy& strategy, IMatchingEngine& engine, Portfolio& portfolio, Logger& logger,
             double tick_size = PRICE_TICK_SIZE);

    void process_event(const MarketEvent& event, bool enable_print = false);
    void process_top_of_book(const std::string& symbol, uint64_t ts, const TopOfBook& top,
                             bool enable_print = false);
    void process_l2_market_view(const L2MarketView& view, bool enable_print = false);
    void process_l2_market_trade(const MarketTrade& trade);
    RunSummary summary() const;

    uint64_t filtered_market_trades() const
    {
        return filtered_market_trades_;
    }

    const ExecutionQualityMetrics& execution_quality() const
    {
        return execution_quality_;
    }

   private:
    void submit_strategy_actions(const std::string& symbol, uint64_t ts, const TopOfBook& top);
    void submit_strategy_actions(const std::string& symbol, uint64_t ts,
                                 std::vector<StrategyOrder> orders);

    std::ofstream& orders_out_;
    std::ofstream& trades_out_;
    IOrderBook& order_book_;
    Strategy& strategy_;
    IMatchingEngine& engine_;
    Portfolio& portfolio_;
    Logger& logger_;
    double tick_size_;
    std::unordered_map<std::string, BboQuote> latest_quotes_;
    std::string market_exchange_;
    std::atomic<uint64_t> next_order_id_{1};
    uint64_t submitted_orders_{0};
    uint64_t submitted_quantity_{0};
    uint64_t cancel_requests_{0};
    uint64_t trade_reports_{0};
    uint64_t trade_report_quantity_{0};
    uint64_t last_buy_queue_ahead_consumed_{0};
    uint64_t last_sell_queue_ahead_consumed_{0};
    uint64_t filtered_market_trades_{0};
    ExecutionQualityMetrics execution_quality_;
    int64_t position_{0};
    uint64_t quote_cycle_{0};
    double last_mid_{0.0};
    uint64_t inventory_samples_{0};
    struct FillObservation
    {
        Side side;
        double price;
        uint64_t start_cycle;
    };
    std::deque<FillObservation> pending_markouts_;
    std::unordered_map<uint64_t, uint64_t> order_start_cycles_;
    struct OrderAudit
    {
        uint64_t start_cycle{0};
        bool filled{false};
    };
    std::unordered_map<uint64_t, OrderAudit> order_audit_;
    std::unordered_set<uint64_t> pending_cancel_orders_;
};
