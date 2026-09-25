#include "strategy_factory.h"

#include <algorithm>

#include "common/types.h"
#include "strategy/active_l2_market_maker.h"
#include "strategy/inventory_aware_l2_market_maker.h"
#include "strategy/l2_market_maker.h"
#include "strategy/market_maker.h"

namespace
{
L2MarketMakerConfig make_flow_l2_config(uint64_t order_size, double tick_size,
                                        int64_t inventory_limit)
{
    return L2MarketMakerConfig{.order_size = order_size,
                               .base_spread = 2.0 * tick_size,
                               .inventory_limit = inventory_limit,
                               .tick_size = tick_size,
                               .imbalance_shift = 2.0 * tick_size,
                               .trade_imbalance_shift = tick_size,
                               .signal_mode = L2SignalMode::Flow,
                               .trade_window = 16ULL,
                               .refresh_price_ticks = 2,
                               .max_quote_age = 50,
                               .toxicity_flow_threshold = 0.65};
}

ActiveL2MarketMakerConfig make_active_l2_config(const L2MarketMakerConfig& base)
{
    return ActiveL2MarketMakerConfig{
        .base = base, .volatility_alpha = 0.25, .pause_after_ticks = 9.0};
}
}  // namespace

std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name,
                                        const InstrumentSpec* instrument,
                                        const FlowAwareMarketMakerConfig& flow_config,
                                        double l1_risk_threshold,
                                        double l1_stress_spread_multiplier,
                                        double l1_minimum_stress_quantity_ratio,
                                        double l1_fee_spread_multiplier)
{
    const uint64_t order_size =
        instrument ? std::max(instrument->min_order_quantity, instrument->quantity_scale / 1000)
                   : 100;
    const double tick_size = instrument ? instrument->tick_size : PRICE_TICK_SIZE;
    const double spread = instrument ? 2.0 * instrument->tick_size : 0.000003;

    if (strategy_name == "naive")
        return std::make_unique<NaiveMarketMaker>(order_size, spread, tick_size);

    const int64_t inventory_limit =
        instrument ? std::max<int64_t>(1, static_cast<int64_t>(instrument->quantity_scale / 10))
                   : 1000;
    if (strategy_name == "passive_l1")
        return std::make_unique<PassiveL1MarketMaker>(order_size, spread, inventory_limit,
                                                      tick_size);
    if (strategy_name == "inventory_aware_l1")
        return std::make_unique<InventoryAwareL1MarketMaker>(order_size, spread, inventory_limit,
                                                            tick_size);
    if (strategy_name == "flow_aware_l1")
        return std::make_unique<FlowAwareL1MarketMaker>(order_size, spread, inventory_limit,
                                                       tick_size, 20, 0.5, flow_config);
    if (strategy_name == "active_l1")
        return std::make_unique<ActiveL1MarketMaker>(order_size, spread, inventory_limit,
                                                     tick_size);
    if (strategy_name == "inventory_aware_l2")
    {
        const auto base_config = make_flow_l2_config(order_size, tick_size, inventory_limit);
        return std::make_unique<InventoryAwareL2MarketMaker>(
            InventoryAwareL2MarketMakerConfig{.base = base_config,
                                               .inventory_limit = inventory_limit,
                                               .max_inventory_shift_ticks = 2.0});
    }
    if (strategy_name == "active_l2" || strategy_name == "adaptive_l2")
    {
        const auto base_config = make_flow_l2_config(order_size, tick_size, inventory_limit);
        return std::make_unique<ActiveL2MarketMaker>(make_active_l2_config(base_config));
    }
    if (strategy_name == "l2" || strategy_name == "passive_l2" ||
        strategy_name == "flow_aware_l2" || strategy_name == "l2_depth" ||
        strategy_name == "l2_baseline" || strategy_name == "l2_micro" ||
        strategy_name == "l2_flow")
    {
        L2SignalMode signal_mode = L2SignalMode::Flow;
        if (strategy_name == "passive_l2" || strategy_name == "l2_baseline")
            signal_mode = L2SignalMode::Baseline;
        if (strategy_name == "l2_depth") signal_mode = L2SignalMode::Depth;
        if (strategy_name == "l2_micro") signal_mode = L2SignalMode::Micro;
        const bool public_flow_strategy = strategy_name == "l2" || strategy_name == "flow_aware_l2";
        if (public_flow_strategy)
            return std::make_unique<L2MarketMaker>(
                make_flow_l2_config(order_size, tick_size, inventory_limit));
        return std::make_unique<L2MarketMaker>(L2MarketMakerConfig{
            .order_size = order_size,
            .base_spread = 2.0 * tick_size,
            .inventory_limit = inventory_limit,
            .tick_size = tick_size,
            .imbalance_shift = 2.0 * tick_size,
            .trade_imbalance_shift = tick_size,
            .signal_mode = signal_mode,
            .trade_window = public_flow_strategy ? 16ULL : 32ULL,
            .refresh_price_ticks = static_cast<uint64_t>(public_flow_strategy ? 2ULL : 1ULL),
            .max_quote_age = static_cast<uint64_t>(public_flow_strategy ? 50ULL : 20ULL),
            .toxicity_flow_threshold = public_flow_strategy ? 0.65 : 2.0,
            .weak_flow_threshold = 0.9,
            .weak_flow_quote_scale = 0.5,
            .weak_flow_spread_shift_ticks = 1.0});
    }
    if (strategy_name == "l1")
    {
        L1MarketMakerConfig config;
        config.order_size = order_size;
        config.base_spread = spread;
        config.inventory_limit = inventory_limit;
        config.tick_size = tick_size;
        config.inventory_risk_threshold = l1_risk_threshold;
        config.stress_spread_multiplier = l1_stress_spread_multiplier;
        config.minimum_stress_quantity_ratio = l1_minimum_stress_quantity_ratio;
        config.fee_spread_multiplier = l1_fee_spread_multiplier;
        return std::make_unique<L1MarketMaker>(config);
    }

    return std::make_unique<OptimizedMarketMaker>(order_size, spread, inventory_limit, tick_size);
}
