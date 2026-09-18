#include "strategy_factory.h"

#include <algorithm>

#include "common/types.h"
#include "strategy/market_maker.h"

std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name,
                                        const InstrumentSpec* instrument)
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
    if (strategy_name == "l1")
    {
        L1MarketMakerConfig config;
        config.order_size = order_size;
        config.base_spread = spread;
        config.inventory_limit = inventory_limit;
        config.tick_size = tick_size;
        return std::make_unique<L1MarketMaker>(config);
    }

    return std::make_unique<OptimizedMarketMaker>(order_size, spread, inventory_limit, tick_size);
}
