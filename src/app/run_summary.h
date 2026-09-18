#pragma once

#include <sstream>
#include <string>

#include "accounting/portfolio.h"
#include "strategy/strategy.h"

struct RunSummary
{
    uint64_t submitted_orders{0};
    uint64_t submitted_quantity{0};
    uint64_t cancel_requests{0};
    uint64_t trade_reports{0};
    uint64_t trade_report_quantity{0};
    double fill_rate{0.0};
    double cancel_rate{0.0};
    size_t working_orders{0};
    PortfolioMetrics portfolio;
    StrategyMetrics strategy;

    std::string to_log_string() const
    {
        std::ostringstream stream;
        stream << "[EXECUTION] submitted_orders=" << submitted_orders
               << " submitted_quantity=" << submitted_quantity
               << " cancel_requests=" << cancel_requests << " trade_reports=" << trade_reports
               << " fill_rate=" << fill_rate << " cancel_rate=" << cancel_rate
               << " trade_report_quantity=" << trade_report_quantity
               << " working_orders=" << working_orders << "\n"
               << "[PORTFOLIO] " << portfolio.to_log_string();
        if (strategy.available) stream << "\n[STRATEGY_METRICS] " << strategy.to_log_string();
        return stream.str();
    }
};
