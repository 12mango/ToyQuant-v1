#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "accounting/portfolio.h"
#include "app/run_summary.h"
#include "common/funding.h"
#include "strategy/strategy.h"

// The run-level measurements that are a storey of their own: the funding series and what its settlements
// did to the account, and the markout of both our own fills and the venue's prints.
//
// They are the two things a market maker measures that are not about one order: funding accrues on the
// position over the clock, and adverse selection is read five quote cycles after a fill, so both hold a
// queue of things that are not resolved yet. That queue, and the horizon it is drained on, is what this
// class is for; the counters, the risk gates and the account itself stay with the coordinator and the
// portfolio, because they are the run rather than a measurement of it.
class RunAccounting
{
   public:
    // The funding history of the instrument being replayed, owned by the caller. The pointer is
    // non-const because the series' consumption cursor advances as the replay passes each settlement.
    // Passing nullptr, the default, is what every recorded run does, and it keeps the summary byte for
    // byte what it was: a run without a series prints no [FUNDING] line at all.
    void set_funding_rates(FundingRateSeries* series)
    {
        funding_rates_ = series;
    }

    // Turn on the measurement the adverse-selection gate needs: what the market's own prints were worth
    // to a passive fill at each distance from the midpoint. Off by default, because it is a per-trade
    // cost and a run that does not ask for the gate must behave and print exactly as it did before.
    void set_markout_cover(bool enabled)
    {
        measure_market_markouts_ = enabled;
    }

    bool market_markouts_enabled() const
    {
        return measure_market_markouts_;
    }

    // Applies every settlement at or before `ts` that the replay has not applied yet. A settlement before
    // the first price is applied to an account that holds nothing yet, so there is no notional to charge
    // and nothing to do; the settlement is still consumed, because the replay has passed it and a later
    // event must not apply it again.
    void apply_due_funding(uint64_t ts, Portfolio& portfolio, double last_mid);

    bool funding_requested() const
    {
        return funding_rates_ != nullptr;
    }

    std::size_t funding_points() const
    {
        return funding_rates_ != nullptr ? funding_rates_->size() : 0;
    }

    uint64_t funding_span_hours() const
    {
        return funding_rates_ != nullptr ? funding_rates_->span_ms() / 3600000 : 0;
    }

    double funding_sum_rate() const
    {
        return funding_rates_ != nullptr ? funding_rates_->sum_rate() : 0.0;
    }

    double funding_annualized_rate() const
    {
        return funding_rates_ != nullptr ? funding_rates_->annualized_rate() : 0.0;
    }

    uint64_t funding_settlements() const
    {
        return funding_settlements_;
    }

    double funding_flow_usd() const
    {
        return funding_flow_usd_;
    }

    // Queues one fill for its markout, resolved five quote cycles later by the same horizon on every path.
    void note_fill(Side side, double price, uint64_t quantity, uint64_t quote_cycle, int bucket);

    // Resolves the fills whose five cycles have closed, adding each to the execution quality buckets.
    void drain_mature_markouts(uint64_t quote_cycle, double last_mid, double tick_size,
                               double unit_notional_usd, double multiplier, uint64_t quantity_scale,
                               ExecutionQualityMetrics& quality);

    // Queues one venue print as if we had been the passive side at its price. A print at or through a
    // price is what a resting quote there would have traded with, so unlike our own fills it exists from
    // the first event rather than from the first one we win.
    void note_market_print(Side passive_side, double price, uint64_t quantity, uint64_t quote_cycle,
                           int bucket);

    // Resolves the prints whose five cycles have closed, and republishes the estimate. Called by the path
    // that advances the quote cycle when the gate was asked for.
    void drain_mature_prints(uint64_t quote_cycle, double last_mid, double tick_size);

    // Recomputes the four quantity-weighted averages and hands them to the strategy, which is the only way
    // the decision can know what being passive at a distance has actually cost in this window.
    void publish_markout_estimate(ExecutionQualityMetrics& quality, Strategy& strategy);

    const MarkoutEstimate& markout_estimate() const
    {
        return markout_estimate_;
    }

    const std::array<double, ExecutionQualityMetrics::kMarkoutBucketCount>& print_markout_average_ticks()
        const
    {
        return print_markout_average_ticks_;
    }

    uint64_t print_samples() const
    {
        return print_samples_;
    }

    // The account's marked equity, sampled once per bucket while the run asked for --metrics. The
    // bucket's last observation replaces the earlier ones, so the series is one point per bucket rather
    // than one per event, and the annualization factor is the bucket count a year holds. Both are
    // properties of the measurement, so they live here beside the markouts rather than in the pipeline.
    static constexpr uint64_t kEquityBucketUs = 60'000'000;

    struct EquityPoint
    {
        uint64_t ts{0};
        double equity{0.0};
    };

    // Records one marked equity at `ts`, keeping only the bucket's last value. Called on every event
    // whose midpoint advances, which is what makes the series cover the clock rather than the fills: a
    // run that never trades still has a curve, and a drawdown is what the account marked, not what it
    // closed.
    void note_equity(uint64_t ts, double equity);

    // The [PERFORMANCE] figures: the drawdown, its duration, the two annualized deviations and the three
    // ratios, all derived from the curve above. Zero for a window with fewer than two samples, because
    // one point has no return and a return series with no interval has no dispersion.
    PerformanceSummary performance_summary() const;

    // The [ROUND_TRIPS] figures, aggregated from the round trips the portfolio booked and the notional the
    // fills traded. Static because the two inputs belong to the account and the fill accounting rather than
    // to a measurement queue: this is arithmetic over what they already hold, not state of its own.
    //
    // A trade is a win when its net PnL is above zero, and every mean is net of the fee the round trip cost:
    // the exit fill's share plus the entry fees the position was carrying. Turnover divides the traded
    // notional by the collateral the run started with.
    static RoundTripSummary round_trip_summary(const std::vector<ClosedTrade>& trades,
                                               double taker_notional_usd,
                                               double total_notional_usd,
                                               double starting_cash_usd);

    // The same distribution over the trades whose position was opened on one side. A market maker's long and
    // short round trips are different populations -- one is filled by sellers, the other by buyers -- so the
    // aggregate can hide a side that never wins, which is the question this split exists to answer.
    static RoundTripSummary side_round_trip_summary(const std::vector<ClosedTrade>& trades,
                                                    exchange::Side side);

    // The horizons the taker markout is reported at, in quote cycles, fixed because the comparison between
    // them is the point: 5 is the horizon the passive markout has always used and the first bucket the
    // Python benchmark measures, 20 is its second, and 60 is the long end where a breakout's drift either
    // arrives or does not.
    static constexpr std::array<uint64_t, 3> kTakerEdgeHorizons{5, 20, 60};

    // One aggressive fill, measured against the midpoint its order decided at. The three slippage figures are
    // computed by the caller because it is the only place that has both prices; this class accumulates and
    // queues the drift.
    struct TakerFillMeasurement
    {
        Side side{Side::Unknown};
        double price{0.0};
        uint64_t quantity{0};
        // Signed as a cost: positive means the fill paid more than the decision price.
        double slippage_ticks{0.0};
        double slippage_bps{0.0};
        double slippage_usd{0.0};
        uint64_t quote_cycle{0};
    };

    // One aggressive order's totals, taken when it ends. `filled_usd` is the filled part against the decision
    // price and `decision_usd` the notional the basis-point figure is measured against; both are in dollars so
    // the opportunity term, which is only known at the horizon, can be added to them.
    struct AggressiveOrder
    {
        Side side{Side::Unknown};
        double decision_mid{0.0};
        uint64_t ordered_quantity{0};
        uint64_t filled_quantity{0};
        double filled_usd{0.0};
        double fees_usd{0.0};
        double decision_usd{0.0};
        uint64_t start_cycle{0};
    };

    // The horizon the passive markouts and the shortfall's opportunity term are resolved at. 5 is the cycle
    // count this project has always used; naming it makes the round trip between a quote and its cost a
    // parameter rather than a constant, which is what a comparison at 20 or 60 cycles needs.
    void set_markout_horizon(uint64_t cycles)
    {
        markout_horizon_cycles_ = cycles == 0 ? 5 : cycles;
    }

    void note_taker_fill(const TakerFillMeasurement& fill);

    void note_aggressive_order(const AggressiveOrder& order);

    // Resolves the taker measurements whose horizon has closed: the drift after each crossing fill at the
    // three fixed horizons, and the opportunity cost of each aggressive order's unfilled remainder. Called
    // from the one place every path advances the quote cycle.
    void drain_taker_measurements(uint64_t quote_cycle, double last_mid, double tick_size,
                                  double unit_notional_usd, double multiplier, uint64_t quantity_scale);

    // The aggregated [TAKER] figures.
    TakerSummary taker_summary() const;

    // ---- The execution line's parent order --------------------------------------------------------------
    // The pipeline records the parent rather than reading it from the strategy, because a measurement that
    // trusted the thing it measures could report a schedule the strategy did not run. The parent is pure
    // configuration: a side, a quantity and a deadline, in market time.
    struct ParentSpec
    {
        Side side{Side::Unknown};
        uint64_t quantity{0};
        uint64_t horizon_us{0};
    };

    void set_parent_order(const ParentSpec& spec);

    bool parent_requested() const
    {
        return parent_requested_;
    }

    // Opens the measurement at the first midpoint the run sees, which is the parent's decision price, and
    // stores the instrument terms that turn a price difference into dollars.
    void start_parent(uint64_t ts, double decision_mid, double unit_notional_usd, double multiplier,
                      uint64_t quantity_scale);

    bool parent_started() const
    {
        return parent_started_;
    }

    bool parent_finished() const
    {
        return parent_finished_;
    }

    // One child order sent while the parent is being worked, and one fill of any child. The fill's USD cost is
    // computed here from the stored decision price, so every child is measured against the same benchmark.
    void note_parent_child();
    void note_parent_fill(Side side, double price, uint64_t quantity, double fee);

    // True once the parent's deadline has passed in market time, which is when the measurement closes.
    bool parent_horizon_reached(uint64_t ts) const
    {
        return parent_started_ && !parent_finished_ && ts >= parent_start_ts_ + parent_spec_.horizon_us;
    }

    // Closes the measurement at the horizon: what the parent never got is charged at the midpoint then, which
    // is the opportunity cost a fill-only account would omit.
    void finish_parent(uint64_t ts, double horizon_mid);

    ExecutionAlgoSummary parent_summary() const;

    // The account's result, split into what completed and what is still open, and into what it was before
    // the venue was paid. The formula lives here rather than in the coordinator's summary literal so that
    // the fields and their definitions are read together: `closed_pnl_usd` is the portfolio's realized
    // PnL, `net_pnl_usd` adds the open position's mark, and the gross pair adds the fees back to each.
    //
    // The two numbers it needs that are the run's rather than this class's -- the maker notional traded
    // and the trade report count -- are passed in, because they are the coordinator's counters.
    static PnlSummary pnl_summary(const Portfolio& portfolio, double maker_notional_usd,
                                  uint64_t trade_reports);

   private:
    // The win/loss half of the distribution, over every trade or over one side. An empty `side` means every
    // trade, and the notional figures stay with the caller because they belong to the account rather than to
    // a side.
    static void fill_round_trip_distribution(const std::vector<ClosedTrade>& trades,
                                             std::optional<exchange::Side> side, RoundTripSummary& summary);

    struct FillObservation
    {
        Side side;
        double price;
        uint64_t quantity{0};
        uint64_t start_cycle;
        // The condition at the fill, recorded here rather than derived when the markout resolves, because
        // a condition read five cycles late would be the future.
        int bucket{-1};
    };

    struct PassivePrint
    {
        Side side{Side::Unknown};
        double price{0.0};
        uint64_t quantity{0};
        uint64_t start_cycle{0};
        int bucket{-1};
    };

    FundingRateSeries* funding_rates_{nullptr};
    uint64_t funding_settlements_{0};
    double funding_flow_usd_{0.0};

    std::deque<FillObservation> pending_markouts_;
    std::deque<PassivePrint> pending_prints_;
    std::array<double, ExecutionQualityMetrics::kMarkoutBucketCount> print_markout_ticks_quantity_{};
    std::array<double, ExecutionQualityMetrics::kMarkoutBucketCount> print_markout_quantity_{};
    // Quantity-weighted averages of the arrays above, recomputed when the estimate is published so the
    // summary can report them without dividing by hand.
    std::array<double, ExecutionQualityMetrics::kMarkoutBucketCount> print_markout_average_ticks_{};
    uint64_t print_samples_{0};
    MarkoutEstimate markout_estimate_{};
    bool measure_market_markouts_{false};
    // The marked account, one point per 60-second bucket, empty unless --metrics asked for it.
    std::vector<EquityPoint> equity_curve_;

    struct TakerObservation
    {
        Side side{Side::Unknown};
        double price{0.0};
        uint64_t quantity{0};
        uint64_t start_cycle{0};
    };

    struct PendingShortfall
    {
        Side side{Side::Unknown};
        double decision_mid{0.0};
        uint64_t unfilled_quantity{0};
        uint64_t start_cycle{0};
    };

    // One queue per horizon rather than one queue with a horizon field, because a fill that is five cycles
    // old and one that is twenty are not in the same order once several fills are in flight, and each queue
    // stays in the order it was filled.
    std::array<std::deque<TakerObservation>, kTakerEdgeHorizons.size()> pending_taker_edges_;
    std::deque<PendingShortfall> pending_shortfalls_;
    double taker_quantity_{0.0};
    double taker_slippage_ticks_quantity_{0.0};
    double taker_slippage_bps_quantity_{0.0};
    double taker_slippage_usd_{0.0};
    uint64_t taker_fills_{0};
    std::array<double, kTakerEdgeHorizons.size()> taker_edge_ticks_quantity_{};
    std::array<double, kTakerEdgeHorizons.size()> taker_edge_quantity_{};
    uint64_t aggressive_orders_{0};
    double shortfall_filled_usd_{0.0};
    double shortfall_fees_usd_{0.0};
    double shortfall_opportunity_usd_{0.0};
    double shortfall_decision_usd_{0.0};
    uint64_t markout_horizon_cycles_{5};

    // The parent order, and the running measurement of working it. `decision_usd` is the notional the basis
    // point figures divide by, fixed at the decision price so a moving market cannot flatter the ratio.
    ParentSpec parent_spec_{};
    bool parent_requested_{false};
    bool parent_started_{false};
    bool parent_finished_{false};
    uint64_t parent_start_ts_{0};
    double parent_decision_mid_{0.0};
    double parent_decision_usd_{0.0};
    double parent_unit_notional_usd_{0.0};
    double parent_multiplier_{1.0};
    uint64_t parent_quantity_scale_{1};
    uint64_t parent_filled_quantity_{0};
    uint64_t parent_children_{0};
    double parent_filled_usd_{0.0};
    double parent_fees_usd_{0.0};
    double parent_horizon_mid_{0.0};
    double parent_opportunity_usd_{0.0};
    double parent_span_seconds_{0.0};
};
