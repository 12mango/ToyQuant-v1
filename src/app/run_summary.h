#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "accounting/portfolio.h"
#include "strategy/strategy.h"

struct ExecutionQualityMetrics
{
    // Markout split by the condition captured at the fill: the side crossed with whether the fill price was at
    // least a tick away from the midpoint then (0 buy far, 1 buy near, 2 sell far, 3 sell near). A market
    // maker's fills are not one population, and the first condition tried for the second dimension, whether the
    // fill added to the position, came back constant under this sizing, which is why the buckets are named for
    // distance rather than for the order flow.
    static constexpr std::size_t kMarkoutBucketCount = 4;
    std::array<double, kMarkoutBucketCount> markout_ticks_quantity_by_bucket{};
    std::array<double, kMarkoutBucketCount> markout_quantity_by_bucket{};
    std::array<uint64_t, kMarkoutBucketCount> markout_count_by_bucket{};
    double captured_edge{0.0};
    double adverse_selection{0.0};
    // Quantity-weighted companions to the two sums above. The sums mix prices with quantities, so the
    // per-unit figures below are the ones to read: captured_edge_ticks_quantity divided by
    // captured_edge_quantity is the average edge per contract in ticks, which compares directly with
    // the half-spread the quote was placed at, and the markout pair is the same for what came back.
    double captured_edge_quantity{0.0};
    double captured_edge_ticks_quantity{0.0};
    double markout_quantity{0.0};
    double markout_ticks_quantity{0.0};
    // The same two quantities in the unit fees are charged in. On an inverse contract a price edge of
    // `e` is worth `quantity * face_value * e / price`, not `quantity * e`, so the money figure has to
    // be converted here rather than assumed. It is the number to read against `fees_paid`: on
    // BTC-PERPETUAL a maker fee of 0.02% on a 10 USD face value costs 0.002 USD per contract, which a
    // quote at the touch earning 0.4 ticks does not cover.
    double captured_edge_usd{0.0};
    double markout_usd{0.0};
    double average_abs_inventory{0.0};
    int64_t max_abs_inventory{0};
    uint64_t inventory_sign_changes{0};
    uint64_t markout_count{0};
    uint64_t total_quote_lifetime{0};
    uint64_t max_quote_lifetime{0};
    uint64_t quote_observations{0};
    uint64_t bbo_quote_observations{0};
    double total_quote_distance{0.0};
    double total_quote_distance_ticks{0.0};
    uint64_t audited_orders{0};
    uint64_t audited_filled_orders{0};
    uint64_t audited_cancelled_orders{0};
    uint64_t cancelled_after_fill_orders{0};
    uint64_t cancelled_before_fill_orders{0};
    // The same orders again, but by how much each of them filled. A commission's minimum per order is a
    // fee the per-share rate does not show, and it is charged on the order instance rather than on the
    // fill, so a strategy whose fills are small pays it on every fill. The bucket bounds are where the
    // two schedules this repository has read stop charging it -- 100 shares at 0.35 with a 0.0035 share
    // rate, and 200 at 1.00 with 0.005 -- which puts both thresholds on a boundary and makes the
    // commission under either convention computable from the counts and the quantity sums.
    static constexpr std::size_t kFillSizeBucketCount = 6;
    uint64_t orders_that_filled{0};
    std::array<uint64_t, kFillSizeBucketCount> filled_order_count_by_size{};
    std::array<uint64_t, kFillSizeBucketCount> filled_order_quantity_by_size{};
    // The floor under a broker's commission, when the schedule has one: how many orders were charged it and
    // how much it added. Zero for every run that sets no floor, which is every run recorded before it could
    // be modelled.
    uint64_t min_commission_orders{0};
    double min_commission_paid{0.0};

    // The histogram as one field, `count/quantity` per bucket, so that the [EXECUTION] line does not grow
    // by twelve numbers to carry it.
    std::string size_histogram() const
    {
        std::string text;
        for (std::size_t index = 0; index < kFillSizeBucketCount; ++index)
        {
            if (index != 0) text += ',';
            text += std::to_string(filled_order_count_by_size[index]) + "/" +
                    std::to_string(filled_order_quantity_by_size[index]);
        }
        return text;
    }
    uint64_t total_order_lifetime_cycles{0};
    // Where each quote went relative to the venue touch. A quote at the touch joins whatever is
    // displayed there; one behind it is protected by the spread and has to wait longer. On this feed
    // the median displayed size is 20,150 contracts while only 7,180 contracts trade at a typical
    // price in a whole 15-minute window, so which of the two a strategy does decides whether it can
    // fill at all: at the touch it competes for the flow, behind it waits for the queue to clear.
    uint64_t quote_at_touch_orders{0};
    uint64_t quote_behind_touch_orders{0};
};

struct TraceRow
{
    uint64_t ts{0};
    std::string event;
    Side side{Side::Unknown};
    double price{0.0};
    int64_t change{0};
    uint64_t queue_ahead{0};
};

// Quantity-weighted markout in ticks for one bucket, or zero when the bucket has no resolved fill. Zero has
// to mean "no evidence" here rather than "no adverse selection", which is why the counts are printed beside it.
inline double markout_bucket_ticks(const ExecutionQualityMetrics& quality, std::size_t bucket)
{
    const double quantity = quality.markout_quantity_by_bucket[bucket];
    return quantity > 0.0 ? quality.markout_ticks_quantity_by_bucket[bucket] / quantity : 0.0;
}

// What the funding input contained and what it did to the account: the file's own arithmetic, and the
// settlements the replay applied. Separated from PortfolioMetrics because a run without a funding series has
// no funding facts at all, and a line of zeros pretending to be a measurement is worse than no line.
struct FundingSummary
{
    // The file: how many settlements it holds, over what span, and the sum and the annualized form of the
    // rates it carries. `sum_rate` is what a long holding one unit of notional paid over the span.
    uint64_t points{0};
    uint64_t span_hours{0};
    double sum_rate{0.0};
    double annualized_rate{0.0};
    // The replay: settlements the event clock passed, and the USD amount they credited to cash. The two
    // differ because a settlement that lands on a flat account is applied and moves nothing.
    uint64_t settlements{0};
    double flow_usd{0.0};
};

// What a run earned and what it paid, split so that quoting and the position left at the end are not read
// as the same thing. `net_pnl_usd` is the closed result plus the open position marked at the last price,
// `closed_pnl_usd` is the closed result alone, and `gross_*` adds the fees back. A market maker whose gross
// PnL is negative loses money before the venue charges anything, which is a different problem from one whose
// gross PnL is positive and whose fees are larger, and only the second of the two can be fixed by a fee tier.
struct PnlSummary
{
    double net_pnl_usd{0.0};          // closed plus the open position's mark, before the split below
    double closed_pnl_usd{0.0};       // what completed, so a position still open is not counted
    double gross_pnl_usd{0.0};        // net, before fees
    double gross_closed_pnl_usd{0.0}; // closed, before fees
    double maker_notional_usd{0.0};   // what the maker fees were charged on
    double net_per_fill_usd{0.0};     // net PnL per trade report
    double gross_closed_per_fill_usd{0.0};
    // The maker rate that would have made this run's total PnL zero, which is the fee question in one number:
    // negative means even a fee of zero leaves a loss and a rebate of that size would be needed. It is exact
    // when the fee does not feed back into the strategy's decisions, which is the case for every run with the
    // fee-covering gate off, and a first-order estimate otherwise, because there the rate changes which
    // quotes are placed.
    double breakeven_maker_rate{0.0};
};

// What the decision layer measured and refused, for a run that asked for the adverse-selection gate or the
// flatten band. Printed only then, because a run that does not ask must produce exactly the output it produced
// before either existed.
struct GateSummary
{
    // Our own fills, quantity weighted, which is what the gate uses: a positive number means a passive fill at
    // that distance was worth money after the print.
    double fill_buy_near_ticks{0.0};
    double fill_buy_far_ticks{0.0};
    double fill_sell_near_ticks{0.0};
    double fill_sell_far_ticks{0.0};
    uint64_t fill_samples{0};
    // The venue's own prints, measured as if we had been the passive side. The first version of the gate used
    // these as a prior, and the record shows why it does not: the two disagree in sign on the mainline window.
    double market_buy_near_ticks{0.0};
    double market_buy_far_ticks{0.0};
    double market_sell_near_ticks{0.0};
    double market_sell_far_ticks{0.0};
    uint64_t market_samples{0};
    // Sides the gate refused because the measured adverse selection was more than the quote could cover.
    uint64_t markout_gate_skips{0};
    // Orders sent to reduce a position that had grown past the flatten band, and the quantity they carried.
    uint64_t flatten_orders{0};
    uint64_t flatten_quantity{0};
};

struct RunSummary
{
    uint64_t submitted_orders{0};
    uint64_t submitted_quantity{0};
    uint64_t cancel_requests{0};
    uint64_t trade_reports{0};
    uint64_t trade_report_quantity{0};
    uint64_t queue_ahead_consumed{0};
    uint64_t buy_queue_ahead_levels_cleared{0};
    uint64_t sell_queue_ahead_levels_cleared{0};
    uint64_t buy_queue_from_quantity_changes{0};
    uint64_t sell_queue_from_quantity_changes{0};
    uint64_t filtered_market_trades{0};
    double fill_rate{0.0};
    double cancel_rate{0.0};
    // Risk and collateral facts. `position_limit` is what the pre-trade gate enforces, 0 when it is
    // off; `starting_cash_usd` is the collateral the documented cash-only model allows and
    // `max_abs_exposure_usd` is how far the marked position went against it.
    int64_t position_limit{0};
    int64_t strategy_net_position{0};
    uint64_t strategy_position_mismatches{0};
    uint64_t risk_rejected_orders{0};
    double starting_cash_usd{0.0};
    double max_abs_exposure_usd{0.0};
    uint64_t exposure_over_collateral_fills{0};
    size_t working_orders{0};
    PortfolioMetrics portfolio;
    // Printed for every run, because whether a run earns anything before the venue is paid is the question
    // the market-making line exists to answer, and a reader should not have to subtract two fields by hand.
    PnlSummary pnl;
    StrategyMetrics strategy;
    ExecutionQualityMetrics execution_quality;
    // Printed only when --funding-rates supplied a series: a [FUNDING] line in a run that has no funding
    // input would change the output of every recorded run and say nothing.
    bool funding_requested{false};
    FundingSummary funding;
    // Printed only when the decision layer's diagnostics were asked for (--markout-cover or --flatten-band):
    // the estimate they carry is a property of the window and the gate is what makes it a decision input.
    bool gate_diagnostics_requested{false};
    GateSummary gate;
    // Which orders are worth tracing, so a reader does not have to guess an id. Printed only when a
    // trace was asked for: a hint about tracing inside a run that traces nothing is noise, and it would
    // also change the output of every ordinary run.
    bool trace_requested{false};
    uint64_t first_fill_order_id{0};
    uint64_t longest_wait_filled_order_id{0};
    uint64_t longest_wait_filled_cycles{0};
    uint64_t longest_wait_working_order_id{0};
    uint64_t longest_wait_working_cycles{0};
    // The queue distribution over orders rather than over fills. `queue_min_fractions_` and
    // `queue_end_fractions_` are per-order ratios: the closest the queue in front came to being clear
    // (0 means the order was first in line at some point) and how much of it was still there when the
    // order left (0 means it filled). A median near 1 on both means most quotes never came close and were
    // pulled anyway, which is the mechanical statement behind a low fill rate.
    uint64_t queue_profile_orders{0};
    uint64_t queue_zero_orders{0};
    double queue_min_fraction_p50{0.0};
    double queue_min_fraction_p10{0.0};
    double queue_end_fraction_p50{0.0};
    // One order's wait, when --trace-order names it: the queue it joined behind, every change in that
    // queue, and the report that ended it. It is the only place in this project where a mechanism is
    // shown as a sequence instead of as a total, which is what makes it the teaching artefact.
    std::vector<TraceRow> queue_trace;

    std::string to_log_string() const
    {
        std::ostringstream stream;
        stream << "[EXECUTION] submitted_orders=" << submitted_orders
               << " submitted_quantity=" << submitted_quantity
               << " cancel_requests=" << cancel_requests << " trade_reports=" << trade_reports
               << " fill_rate=" << fill_rate << " cancel_rate=" << cancel_rate
               << " trade_report_quantity=" << trade_report_quantity
               << " queue_ahead_consumed=" << queue_ahead_consumed
               << " buy_queue_ahead_levels_cleared=" << buy_queue_ahead_levels_cleared
               << " sell_queue_ahead_levels_cleared=" << sell_queue_ahead_levels_cleared
               << " buy_queue_from_quantity_changes="
               << buy_queue_from_quantity_changes
               << " sell_queue_from_quantity_changes="
               << sell_queue_from_quantity_changes
               << " filtered_market_trades=" << filtered_market_trades
               << " quote_observations=" << execution_quality.quote_observations
               << " bbo_quote_observations=" << execution_quality.bbo_quote_observations
               << " total_quote_distance=" << execution_quality.total_quote_distance
               << " total_quote_distance_ticks="
               << execution_quality.total_quote_distance_ticks
               << " audited_orders=" << execution_quality.audited_orders
               << " audited_filled_orders=" << execution_quality.audited_filled_orders
               << " audited_cancelled_orders=" << execution_quality.audited_cancelled_orders
               << " cancelled_after_fill_orders="
               << execution_quality.cancelled_after_fill_orders
               << " cancelled_before_fill_orders="
               << execution_quality.cancelled_before_fill_orders
               << " total_order_lifetime_cycles="
               << execution_quality.total_order_lifetime_cycles
               << " quote_at_touch_orders=" << execution_quality.quote_at_touch_orders
               << " quote_behind_touch_orders=" << execution_quality.quote_behind_touch_orders
               << " queue_profile_orders=" << queue_profile_orders
               << " queue_zero_orders=" << queue_zero_orders
               << " queue_min_fraction_p50=" << queue_min_fraction_p50
               << " queue_min_fraction_p10=" << queue_min_fraction_p10
               << " queue_end_fraction_p50=" << queue_end_fraction_p50
               << " captured_edge=" << execution_quality.captured_edge
               << " captured_edge_per_unit_ticks="
               << (execution_quality.captured_edge_quantity > 0.0
                       ? execution_quality.captured_edge_ticks_quantity /
                             execution_quality.captured_edge_quantity
                       : 0.0)
               << " captured_edge_usd=" << execution_quality.captured_edge_usd
               << " adverse_selection=" << execution_quality.adverse_selection
               << " markout_per_unit_ticks="
               << (execution_quality.markout_quantity > 0.0
                       ? execution_quality.markout_ticks_quantity /
                             execution_quality.markout_quantity
                       : 0.0)
               << " markout_usd=" << execution_quality.markout_usd
               << " markout_count=" << execution_quality.markout_count
               << " markout_buy_far_ticks=" << markout_bucket_ticks(execution_quality, 0)
               << " markout_buy_far_count=" << execution_quality.markout_count_by_bucket[0]
               << " markout_buy_near_ticks=" << markout_bucket_ticks(execution_quality, 1)
               << " markout_buy_near_count=" << execution_quality.markout_count_by_bucket[1]
               << " markout_sell_far_ticks=" << markout_bucket_ticks(execution_quality, 2)
               << " markout_sell_far_count=" << execution_quality.markout_count_by_bucket[2]
               << " markout_sell_near_ticks=" << markout_bucket_ticks(execution_quality, 3)
               << " markout_sell_near_count=" << execution_quality.markout_count_by_bucket[3]
               << " avg_abs_inventory=" << execution_quality.average_abs_inventory
               << " max_abs_inventory=" << execution_quality.max_abs_inventory
               << " inventory_sign_changes=" << execution_quality.inventory_sign_changes
               << " total_quote_lifetime=" << execution_quality.total_quote_lifetime
               << " max_quote_lifetime=" << execution_quality.max_quote_lifetime
               << " position_limit=" << position_limit
               << " risk_rejected_orders=" << risk_rejected_orders
               << " strategy_net_position=" << strategy_net_position
               << " strategy_position_mismatches=" << strategy_position_mismatches
               << " starting_cash_usd=" << starting_cash_usd
               << " max_abs_exposure_usd=" << max_abs_exposure_usd
               << " exposure_over_collateral_fills=" << exposure_over_collateral_fills
               << " working_orders=" << working_orders
               << " orders_that_filled=" << execution_quality.orders_that_filled
               << " filled_orders_by_size=" << execution_quality.size_histogram()
               << " min_commission_orders=" << execution_quality.min_commission_orders
               << " min_commission_paid=" << execution_quality.min_commission_paid << "\n"
               << "[PORTFOLIO] " << portfolio.to_log_string()
               << "\n[PNL] gross_pnl_usd=" << pnl.gross_pnl_usd
               << " net_pnl_usd=" << pnl.net_pnl_usd
               << " gross_closed_pnl_usd=" << pnl.gross_closed_pnl_usd
               << " closed_pnl_usd=" << pnl.closed_pnl_usd
               << " maker_notional_usd=" << pnl.maker_notional_usd
               << " net_per_fill_usd=" << pnl.net_per_fill_usd
               << " gross_closed_per_fill_usd=" << pnl.gross_closed_per_fill_usd
               << " breakeven_maker_rate=" << pnl.breakeven_maker_rate;
        if (strategy.available) stream << "\n[STRATEGY_METRICS] " << strategy.to_log_string();
        if (funding_requested)
            stream << "\n[FUNDING] funding_points=" << funding.points
                   << " funding_span_hours=" << funding.span_hours
                   << " funding_sum_rate=" << funding.sum_rate
                   << " funding_annualized_rate=" << funding.annualized_rate
                   << " funding_settlements=" << funding.settlements
                   << " funding_flow_usd=" << funding.flow_usd;
        if (gate_diagnostics_requested)
            stream << "\n[GATE] fill_markout_buy_near_ticks=" << gate.fill_buy_near_ticks
                   << " fill_markout_buy_far_ticks=" << gate.fill_buy_far_ticks
                   << " fill_markout_sell_near_ticks=" << gate.fill_sell_near_ticks
                   << " fill_markout_sell_far_ticks=" << gate.fill_sell_far_ticks
                   << " fill_markout_samples=" << gate.fill_samples
                   << " market_markout_buy_near_ticks=" << gate.market_buy_near_ticks
                   << " market_markout_buy_far_ticks=" << gate.market_buy_far_ticks
                   << " market_markout_sell_near_ticks=" << gate.market_sell_near_ticks
                   << " market_markout_sell_far_ticks=" << gate.market_sell_far_ticks
                   << " market_markout_samples=" << gate.market_samples
                   << " markout_gate_skips=" << gate.markout_gate_skips
                   << " flatten_orders=" << gate.flatten_orders
                   << " flatten_quantity=" << gate.flatten_quantity;
        if (trace_requested)
        {
            stream << "\n[TRACE_HINTS] first_fill_order_id=" << first_fill_order_id
                   << " longest_wait_filled_order_id=" << longest_wait_filled_order_id
                   << " longest_wait_filled_cycles=" << longest_wait_filled_cycles
                   << " longest_wait_working_order_id=" << longest_wait_working_order_id
                   << " longest_wait_working_cycles=" << longest_wait_working_cycles;
        }
        for (const auto& row : queue_trace)
        {
            stream << "\n[QUEUE_TRACE] ts=" << row.ts << " event=" << row.event
                   << " side=" << (row.side == Side::Buy ? "buy" : "sell") << " price=" << row.price
                   << " change=" << row.change << " queue_ahead=" << row.queue_ahead;
        }
        return stream.str();
    }
};
