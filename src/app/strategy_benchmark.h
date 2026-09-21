#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "strategy/market_maker.h"

struct StrategyBenchmarkResult
{
    std::string strategy_name;
    uint64_t submitted_orders{0};
    uint64_t submitted_quantity{0};
    uint64_t trade_reports{0};
    uint64_t filled_quantity{0};
    uint64_t cancel_requests{0};
    int64_t net_position{0};
    double fill_rate{0.0};
    double cancel_rate{0.0};
    double realized_pnl{0.0};
    double equity{0.0};
    double fees_paid{0.0};
    size_t working_orders{0};
    uint64_t stale_trades{0};
    uint64_t dislocated_trades{0};
    double max_trade_deviation_bps{0.0};
};

std::vector<StrategyBenchmarkResult> run_strategy_benchmark(const std::string& trades_path,
                                                            const std::string& quotes_path,
                                                            const std::string& symbol,
                                                            uint64_t quantity_scale = 1000000,
                                                            const FlowAwareMarketMakerConfig& flow_config = {},
                                                            double l1_risk_threshold = 0.60,
                                                            double l1_stress_spread_multiplier = 2.5,
                                                            double l1_minimum_stress_quantity_ratio = 0.10,
                                                            double l1_fee_spread_multiplier = 0.40);
