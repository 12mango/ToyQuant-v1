#pragma once

#include <memory>
#include <string>

#include "common/instrument_spec.h"
#include "strategy/strategy.h"

std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name,
                                        const InstrumentSpec* instrument = nullptr);
