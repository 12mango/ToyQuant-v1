#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

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
};
