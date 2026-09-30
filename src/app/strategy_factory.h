#pragma once

#include <memory>
#include <string>

#include "common/instrument_spec.h"
#include "legacy/l1_market_maker_config.h"
#include "strategy/strategy.h"

// Everything make_strategy can be told. These used to be eleven positional arguments, seven of them
// doubles, so a call site could transpose two of them and still compile: the four L1 knobs are all
// fractions and the two overrides are both counts. Named members make that mistake impossible to write.
struct StrategyFactoryConfig
{
    const InstrumentSpec* instrument = nullptr;

    // L1 flow strategies.
    FlowAwareMarketMakerConfig flow_config{};
    double l1_risk_threshold = 0.60;
    double l1_stress_spread_multiplier = 2.5;
    double l1_minimum_stress_quantity_ratio = 0.10;
    double l1_fee_spread_multiplier = 0.40;

    // Overrides handed to the L2 configs, where 0 means "keep the strategy's own default".
    uint64_t order_size_override = 0;
    int64_t inventory_limit_override = 0;
    double base_spread_ticks_override = 0.0;
    uint64_t refresh_price_ticks_override = 0;
    // The decision layer's expected-value gate: extra ticks a quote has to cover on top of the maker fee.
    // 0 leaves the gate off, which is what every run recorded in the documents asks for.
    double edge_cover_ticks_override = 0.0;
    // The conditional rule: quote only when the top of book is at least this many ticks wide. 0 leaves it off.
    double min_spread_ticks_override = 0.0;
};

std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name,
                                        const StrategyFactoryConfig& config = {});
