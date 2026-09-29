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
#include "utils/stage_profiler.h"

class Pipeline
{
   public:
    Pipeline(std::ofstream& orders_out, std::ofstream& trades_out, IOrderBook& order_book,
             Strategy& strategy, IMatchingEngine& engine, Portfolio& portfolio,
             double tick_size = PRICE_TICK_SIZE);

    void process_event(const MarketEvent& event);
    void process_top_of_book(const std::string& symbol, uint64_t ts, const TopOfBook& top);
    void process_l2_market_view(const L2MarketView& view);
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

    // Pre-trade position limit in quantity units. 0, the default, disables the gate, and the run then
    // reports how far the position went and how often the marked exposure passed the collateral
    // instead of blocking anything. A positive value refuses any order that would leave the position,
    // counting resting orders that could still fill, outside +/- the limit.
    void set_position_limit(int64_t limit)
    {
        position_limit_ = limit;
    }

    // Collateral report inputs: the cash the run started with, the USD value of one quantity unit
    // where 0 means the traded price is the unit value (the spot case), and the quantity scale the
    // instrument counts in. The scale matters because quantities are raw instrument units: one BTCUSDT
    // unit is a millionth of a BTC, so a notional computed from raw quantities is a million times too
    // large, which is exactly what the first version of these fields reported.
    void set_collateral(double starting_cash_usd, double unit_notional_usd,
                        uint64_t quantity_scale = 1)
    {
        starting_cash_usd_ = starting_cash_usd;
        unit_notional_usd_ = unit_notional_usd;
        quantity_scale_ = quantity_scale == 0 ? 1 : quantity_scale;
    }

    // Optional per-stage timing. The profiler is owned by the caller and has to outlive
    // the pipeline. Passing nullptr, which is the default, keeps the hot path unchanged.
    void set_profiler(StageProfiler* profiler)
    {
        profiler_ = profiler;
    }

    // Names one order whose queue is worth watching. 0, the default, traces nothing. A trace turns a
    // total into a sequence: the strategy's intent, the queue the order joined behind, every change in
    // that queue, and the fill or the cancel that ended it. It is the only view this project offers of a
    // mechanism rather than of its sum, and it is what makes the queue model legible instead of asserted.
    void set_trace_order(uint64_t order_id)
    {
        trace_order_id_ = order_id;
    }

   private:
    void profiler_begin(Stage stage)
    {
        if (profiler_ != nullptr) profiler_->begin(stage);
    }

    void profiler_end(Stage stage)
    {
        if (profiler_ != nullptr) profiler_->end(stage);
    }

    void submit_strategy_actions(const std::string& symbol, uint64_t ts, const TopOfBook& top);
    void submit_strategy_actions(const std::string& symbol, uint64_t ts,
                                 std::vector<StrategyOrder> orders);

    // The queue the traced order is waiting behind, sampled after an event that could have changed it.
    // Consecutive samples that do not move are dropped, so a row means the queue actually changed.
    void record_trace_sample(const char* event, uint64_t ts);
    void record_trace_report(const ExecutionReport& report);

    std::ofstream& orders_out_;
    std::ofstream& trades_out_;
    IOrderBook& order_book_;
    Strategy& strategy_;
    IMatchingEngine& engine_;
    Portfolio& portfolio_;
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
    int64_t position_limit_{0};
    // Quantity reserved by resting orders, so the gate accounts for what could still fill rather than
    // only for the position as it stands.
    int64_t pending_buy_quantity_{0};
    int64_t pending_sell_quantity_{0};
    uint64_t risk_rejected_orders_{0};
    double starting_cash_usd_{0.0};
    double unit_notional_usd_{0.0};
    uint64_t quantity_scale_{1};
    double max_abs_exposure_usd_{0.0};
    uint64_t exposure_over_collateral_fills_{0};
    uint64_t strategy_position_mismatches_{0};
    double last_mid_{0.0};
    uint64_t inventory_samples_{0};
    struct FillObservation
    {
        Side side;
        double price;
        uint64_t quantity{0};
        uint64_t start_cycle;
    };
    std::deque<FillObservation> pending_markouts_;
    std::unordered_map<uint64_t, uint64_t> order_start_cycles_;
    struct OrderAudit
    {
        uint64_t start_cycle{0};
        bool filled{false};
        // Queue profile. The level belongs to our order while it rests, so the queue behind it is measured
        // when the order arrives and again before any cancel, never after it leaves: the engine erases a
        // level once no order rests in it, and a reading taken after that would report an empty queue for a
        // level that was not empty. `min_queue_ahead` is sampled after every event, which is what turns one
        // order's trace into a distribution over all of them.
        bool profiling{false};
        Side side{Side::Unknown};
        // Braced like every other member, because GCC's -Wmissing-field-initializers exempts members that
        // have a default initializer and reports the one that does not.
        std::string symbol{};
        double price{0.0};
        uint64_t queue_at_rest{0};
        uint64_t min_queue_ahead{0};
        uint64_t queue_at_end{0};
    };
    std::unordered_map<uint64_t, OrderAudit> order_audit_;
    // One entry per order that rested and died, holding the two ratios worth aggregating: how close the
    // queue in front came, and how much of it was still there when the order left.
    std::vector<double> queue_min_fractions_;
    std::vector<double> queue_end_fractions_;
    uint64_t queue_profile_orders_{0};
    uint64_t queue_zero_orders_{0};

    // Samples the queue in front of every order we are resting. Called after each event that could have
    // changed such a level; the map holds only open orders, so this is a handful of lookups per event.
    void sample_resting_queues();

    // Unwinds the pipeline's own bookkeeping for an order the engine refused. Nothing rests, so no report
    // will ever arrive for it, and the strategy is told what a venue would tell it rather than left quoting
    // around a level it is not in.
    void handle_rejected_order(const exchange::Order& exchange_order, const StrategyOrder& strategy_order,
                               uint64_t ts);
    std::unordered_set<uint64_t> pending_cancel_orders_;
    // Trace state. The level stays unknown until the traced order has been submitted, because until then
    // there is no price to look up and a trace that guessed one would be worse than no trace.
    uint64_t trace_order_id_{0};
    // The clock of the event being processed. An order's own reports carry the order's timestamp, so a
    // trace that read report.ts would print a cancel at the moment the order was submitted and the
    // sequence would read backwards. The row has to be timed by the event that caused it.
    uint64_t current_ts_{0};
    bool trace_level_known_{false};
    std::string trace_symbol_;
    Side trace_side_{Side::Unknown};
    double trace_price_{0.0};
    uint64_t trace_last_queue_{0};
    std::vector<TraceRow> trace_rows_;
    // Which orders turned out to be worth tracing. A reader cannot know an id before the run, and the
    // first few orders are hostile samples: they die in the same millisecond they arrive. These are the
    // orders that actually waited, so a second run can name one of them.
    uint64_t first_fill_order_id_{0};
    uint64_t longest_wait_filled_order_id_{0};
    uint64_t longest_wait_filled_cycles_{0};
    StageProfiler* profiler_{nullptr};
};
