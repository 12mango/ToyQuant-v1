#pragma once

#include <memory>
#include <string>

#include "app/trend_params.h"
#include "common/instrument_spec.h"
#include "lines/legacy/l1_market_maker_config.h"
#include "strategy/strategy.h"

// Everything only the frozen L1 line reads. Five flat fields became one member so that the live
// configuration does not sit beside knobs it never touches, and so that the one place an L1 type is
// visible in this header is a single name a reader can skip. `FlowAwareMarketMakerConfig` still comes
// from `lines/legacy/`, because the type belongs to that line: moving it here would be a change to the
// frozen code rather than to this seam.
struct L1StrategyConfig
{
    FlowAwareMarketMakerConfig flow_config{};
    double risk_threshold = 0.60;
    double stress_spread_multiplier = 2.5;
    double minimum_stress_quantity_ratio = 0.10;
    double fee_spread_multiplier = 0.40;
};

// Everything make_strategy can be told. These used to be eleven positional arguments, seven of them
// doubles, so a call site could transpose two of them and still compile: the four L1 knobs are all
// fractions and the two overrides are both counts. Named members make that mistake impossible to write.
struct StrategyFactoryConfig
{
    const InstrumentSpec* instrument = nullptr;

    // The frozen L1 line's own settings. A run that selects an L1 name reads them; nothing else does.
    L1StrategyConfig l1{};

    // Overrides handed to the L2 configs, where 0 means "keep the strategy's own default".
    uint64_t order_size_override = 0;
    int64_t inventory_limit_override = 0;
    double base_spread_ticks_override = 0.0;
    // The inventory-aware strategy's quote shift at a full inventory, in ticks. 0 leaves its own default.
    double inventory_shift_ticks_override = 0.0;
    uint64_t refresh_price_ticks_override = 0;
    // The decision layer's expected-value gate: extra ticks a quote has to cover on top of the maker fee.
    // 0 leaves the gate off, which is what every run recorded in the documents asks for.
    double edge_cover_ticks_override = 0.0;
    // The adverse-selection gate: the cover a quote needs includes what passive fills at its distance have
    // actually cost in this window, as measured from the venue's own prints. Off by default.
    bool markout_cover = false;
    // Inventory discipline: above this many quantity units the L2 decision layer reduces the position with a
    // taker order at the touch instead of carrying it. 0 leaves every position held to the end.
    int64_t flatten_band_units = 0;
    // The conditional rule: quote only when the top of book is at least this many ticks wide. 0 leaves it off.
    double min_spread_ticks_override = 0.0;
    // The non-market-making signal, from --trend-params. `set` false leaves the strategy's own defaults.
    TrendParams trend{};
};

std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name,
                                        const StrategyFactoryConfig& config = {});
