#pragma once

#include <atomic>
#include <deque>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "app/fill_accounting.h"
#include "app/order_ledger.h"
#include "app/risk_pipeline.h"
#include "app/run_accounting.h"
#include "app/run_summary.h"
#include "common/funding.h"
#include "exchange/matching_engine.h"
#include "common/market_event.h"
#include "orderbook/top_of_book.h"
#include "strategy/strategy.h"
#include "utils/stage_profiler.h"

class Pipeline
{
   public:
    // The coordinator takes what it drives and nothing else. It used to take an `IOrderBook&` as well,
    // for the one branch that turns a `BboQuote` into a top of book, which made every caller build a book:
    // the L2 path built one it never touched. The conversion is the pipeline's own now.
    Pipeline(std::ofstream& orders_out, std::ofstream& trades_out, Strategy& strategy,
             IMatchingEngine& engine, Portfolio& portfolio, double tick_size = PRICE_TICK_SIZE);

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
        risk_pipeline_.set_position_limit(limit);
    }

    // The hard exposure cap in USD and the loss at which the run stops quoting. See the flags of the same
    // names: the cap is in dollars rather than shares because the same share count is three times the
    // exposure on a thousand dollar share, and the switch stops quoting rather than flattening.
    void set_hard_exposure_limit_usd(double limit)
    {
        risk_pipeline_.set_hard_exposure_limit_usd(limit);
    }

    void set_kill_switch_loss_usd(double loss)
    {
        risk_pipeline_.set_kill_switch_loss_usd(loss);
    }

    // The two controls the non-market-making line added: a price band around the current midpoint, in
    // basis points, and a token bucket on submissions. Both are off at zero.
    void set_fat_finger_bps(double bps)
    {
        risk_pipeline_.set_fat_finger_bps(bps);
    }

    void set_order_rate_limit(double orders_per_second, uint64_t burst)
    {
        risk_pipeline_.set_order_rate_limit(orders_per_second, burst);
    }

    // Applies one time in force to every order the strategy submits. `override_order` is false unless the
    // run named the flag, which leaves a strategy's own choice intact by default.
    void set_time_in_force(TimeInForce tif, bool override_order)
    {
        time_in_force_ = tif;
        time_in_force_override_ = override_order;
    }

    bool kill_switch_triggered() const
    {
        return risk_pipeline_.kill_switch_triggered();
    }

    // The share schedule terms a broker charges at order level rather than per fill: the commission per unit,
    // and the floor one order's commission has under it. Both zero by default, so a run that sets nothing
    // charges exactly what it charged before the floor could be modelled. The commission is the broker's own
    // per-share term and not the venue's rate, because a broker's floor is a floor on its commission.
    void set_schedule_terms(double commission_per_unit, double min_commission_per_order)
    {
        commission_per_unit_ = commission_per_unit;
        min_commission_per_order_ = min_commission_per_order;
    }

    // Collateral report inputs: the cash the run started with, the USD value of one quantity unit
    // where 0 means the traded price is the unit value (the spot case), and the quantity scale the
    // instrument counts in. The scale matters because quantities are raw instrument units: one BTCUSDT
    // unit is a millionth of a BTC, so a notional computed from raw quantities is a million times too
    // large, which is exactly what the first version of these fields reported.
    void set_collateral(double starting_cash_usd, double unit_notional_usd,
                        uint64_t quantity_scale = 1, double multiplier = 1.0)
    {
        starting_cash_usd_ = starting_cash_usd;
        unit_notional_usd_ = unit_notional_usd;
        quantity_scale_ = quantity_scale == 0 ? 1 : quantity_scale;
        multiplier_ = multiplier;
    }

    // Optional per-stage timing. The profiler is owned by the caller and has to outlive
    // the pipeline. Passing nullptr, which is the default, keeps the hot path unchanged.
    void set_profiler(StageProfiler* profiler)
    {
        profiler_ = profiler;
    }

    // The funding history of the instrument being replayed, owned by the caller like the profiler. The
    // pointer is non-const because the pipeline advances the series' consumption cursor as the replay passes
    // each settlement; the series itself is unchanged, so a caller can read it afterwards.
    //
    // Passing nullptr, which is the default, is what every recorded run does, and it keeps the summary byte
    // for byte what it was: a run without a funding series prints no [FUNDING] line at all.
    //
    // With a series, the pipeline applies one settlement each time the event clock passes one, on the
    // positions as they stand and at the last price the run has seen for the instrument. The position is
    // exact — no event between the settlement and the event that follows it could have changed it — and
    // the mark is the nearest causal one, since the settlement instant itself is not in the event stream.
    void set_funding_rates(FundingRateSeries* series)
    {
        accounting_.set_funding_rates(series);
    }

    // Turn on the measurement the adverse-selection gate needs: what the market's own prints were worth to a
    // passive fill at each distance from the midpoint. Off by default, because it is a per-trade cost and a run
    // that does not ask for the gate must behave and print exactly as it did before.
    void set_markout_cover(bool enabled)
    {
        accounting_.set_markout_cover(enabled);
    }

    // Print the [GATE] line: the measured markouts and the counters the decision layer keeps about what it
    // refused and what it flattened. Set by a caller that asked for either gate, so a run that asks for neither
    // produces exactly the output it produced before either existed.
    void set_gate_diagnostics(bool enabled)
    {
        gate_diagnostics_ = enabled;
    }

    // Names one order whose queue is worth watching. 0, the default, traces nothing. A trace turns a
    // total into a sequence: the strategy's intent, the queue the order joined behind, every change in
    // that queue, and the fill or the cancel that ended it. It is the only view this project offers of a
    // mechanism rather than of its sum, and it is what makes the queue model legible instead of asserted.
    void set_trace_order(uint64_t order_id)
    {
        ledger_.set_trace_order(order_id);
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

    // One named entry point per MarketEvent alternative. The dispatch itself stays in process_event; these
    // overloads keep a new event type or a new non-MM path from having to edit an if-constexpr chain.
    void handle_event(const MarketTrade& trade);
    void handle_event(const BboQuote& quote);
    void handle_event(const MarketDepthSnapshot& snapshot);
    void handle_event(const IncrementalBookBatch& batch);
    void submit_strategy_actions(uint64_t ts, std::vector<StrategyOrder> orders);
    // The report fan-out: ledger, portfolio, risk reservation, execution metrics and strategy callbacks.
    // It is named here rather than buried in the constructor's callback lambda so the order lifecycle can
    // be read as one path.
    void handle_report(const ExecutionReport& report);
    // One order's path from risk to the engine. It is separate from the loop so a future non-MM
    // executor can reuse the same risk, ledger and rejection handling without copying this body.
    void submit_order(StrategyOrder& order, uint64_t ts, const BboQuote* quote);
    // Quote-placement statistics are a market-making concern, not part of generic order submission.
    void record_quote_observation(const StrategyOrder& order, const BboQuote& quote);

    OrderLedger ledger_;
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
    TimeInForce time_in_force_{TimeInForce::Gtc};
    bool time_in_force_override_{false};
    uint64_t last_buy_queue_ahead_consumed_{0};
    uint64_t last_sell_queue_ahead_consumed_{0};
    uint64_t filtered_market_trades_{0};
    ExecutionQualityMetrics execution_quality_;
    uint64_t quote_cycle_{0};
    // The pre-trade controls in one object, so the order path and the summary read the same counters and
    // the order of the checks is a property of the risk pipeline rather than of this loop.
    RiskPipeline risk_pipeline_;
    double starting_cash_usd_{0.0};
    double unit_notional_usd_{0.0};
    // USD value of one price unit in one quantity unit, from the instrument: 1 unless the contract has a
    // multiplier, which is how a futures notional is five times its index price rather than equal to it.
    double multiplier_{1.0};
    uint64_t quantity_scale_{1};
    // The two schedule terms charged per order rather than per fill. See set_schedule_terms.
    double commission_per_unit_{0.0};
    double min_commission_per_order_{0.0};
    uint64_t strategy_position_mismatches_{0};
    double last_mid_{0.0};
    uint64_t inventory_samples_{0};

    // The run's own measurements: the funding series, and the markout of our fills and of the venue's
    // prints. Both are measured over the clock rather than per order, so they hold a queue of what is not
    // resolved yet and the horizon it is drained on.
    RunAccounting accounting_;
    // The state one of our own fills changes. Its inputs stay in Pipeline because they also configure
    // the engine and the report; the mutable state lives here.
    FillAccounting fill_accounting_;
    bool gate_diagnostics_{false};

    // Unwinds the pipeline's own bookkeeping for an order the engine refused. Nothing rests, so no report
    // will ever arrive for it, and the strategy is told what a venue would tell it rather than left quoting
    // around a level it is not in.
    void handle_rejected_order(const exchange::Order& exchange_order, const StrategyOrder& strategy_order,
                               uint64_t ts);
    // The clock of the event being processed. An order's own reports carry the order's timestamp, so a
    // trace that read report.ts would print a cancel at the moment the order was submitted and the
    // sequence would read backwards. The row has to be timed by the event that caused it.
    uint64_t current_ts_{0};
    StageProfiler* profiler_{nullptr};
};
