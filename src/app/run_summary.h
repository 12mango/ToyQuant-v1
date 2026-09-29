#pragma once

#include <sstream>
#include <string>
#include <vector>

#include "accounting/portfolio.h"
#include "strategy/strategy.h"

struct ExecutionQualityMetrics
{
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
    StrategyMetrics strategy;
    ExecutionQualityMetrics execution_quality;
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
               << " working_orders=" << working_orders << "\n"
               << "[PORTFOLIO] " << portfolio.to_log_string();
        if (strategy.available) stream << "\n[STRATEGY_METRICS] " << strategy.to_log_string();
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
