#include "strategy_factory.h"

#include <algorithm>

#include "common/types.h"
#include "strategy/market_maker.h"

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
        instrument ? static_cast<int64_t>(instrument->quantity_scale / 10) : 1000;
    if (strategy_name == "passive_l1")
        return std::make_unique<PassiveL1MarketMaker>(order_size, spread, inventory_limit,
                                                      tick_size);
    if (strategy_name == "inventory_aware_l1")
        return std::make_unique<InventoryAwareL1MarketMaker>(order_size, spread, inventory_limit,
                                                            tick_size);
    if (strategy_name == "flow_aware_l1")
        return std::make_unique<FlowAwareL1MarketMaker>(order_size, spread, inventory_limit,
                                                       tick_size, 20, 0.5, flow_config);
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
