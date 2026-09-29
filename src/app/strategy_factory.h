#pragma once

#include <memory>
#include <string>

#include "common/instrument_spec.h"
#include "legacy/l1_market_maker_config.h"
#include "strategy/strategy.h"

std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name,
                                        const InstrumentSpec* instrument = nullptr,
                                        const FlowAwareMarketMakerConfig& flow_config = {},
                                        double l1_risk_threshold = 0.60,
                                        double l1_stress_spread_multiplier = 2.5,
                                        double l1_minimum_stress_quantity_ratio = 0.10,
                                        double l1_fee_spread_multiplier = 0.40,
                                        uint64_t order_size_override = 0,
                                        int64_t inventory_limit_override = 0,
                                        double base_spread_ticks_override = 0.0,
                                        uint64_t refresh_price_ticks_override = 0);
