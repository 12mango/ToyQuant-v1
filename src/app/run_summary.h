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
};

struct RunSummary
{
    uint64_t submitted_orders{0};
    uint64_t submitted_quantity{0};
    uint64_t cancel_requests{0};
    uint64_t trade_reports{0};
    uint64_t trade_report_quantity{0};
    uint64_t queue_ahead_consumed{0};
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
               << " filtered_market_trades=" << filtered_market_trades
               << " working_orders=" << working_orders << "\n"
               << "[PORTFOLIO] " << portfolio.to_log_string();
        if (strategy.available) stream << "\n[STRATEGY_METRICS] " << strategy.to_log_string();
        return stream.str();
    }
};
