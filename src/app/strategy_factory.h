#pragma once

#include <memory>
#include <string>

#include "common/instrument_spec.h"
#include "strategy/market_maker.h"
#include "strategy/strategy.h"

std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name,
                                        const InstrumentSpec* instrument = nullptr,
                                        const FlowAwareMarketMakerConfig& flow_config = {},
                                        double l1_risk_threshold = 0.60,
                                        double l1_stress_spread_multiplier = 2.5,
                                        double l1_minimum_stress_quantity_ratio = 0.10,
                                        double l1_fee_spread_multiplier = 0.40);
