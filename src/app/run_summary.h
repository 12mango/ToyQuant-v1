#pragma once

#include <sstream>
#include <string>

#include "accounting/portfolio.h"
#include "strategy/strategy.h"

struct ExecutionQualityMetrics
{
    double captured_edge{0.0};
    double adverse_selection{0.0};
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
    size_t working_orders{0};
    PortfolioMetrics portfolio;
    StrategyMetrics strategy;
    ExecutionQualityMetrics execution_quality;

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
               << " working_orders=" << working_orders << "\n"
               << "[PORTFOLIO] " << portfolio.to_log_string();
        if (strategy.available) stream << "\n[STRATEGY_METRICS] " << strategy.to_log_string();
        return stream.str();
    }
};
