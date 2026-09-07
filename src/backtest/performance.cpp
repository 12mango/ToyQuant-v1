#include "backtest/performance.h"

#include <cstdlib>

namespace metrics
{
double compute_fill_rate(uint64_t submitted, uint64_t filled)
{
    if (submitted == 0) return 0.0;
    return static_cast<double>(filled) / static_cast<double>(submitted);
}

double compute_cancel_rate(uint64_t submitted, uint64_t cancelled)
{
    if (submitted == 0) return 0.0;
    return static_cast<double>(cancelled) / static_cast<double>(submitted);
}

double compute_max_drawdown(const std::vector<double>& equity)
{
    if (equity.empty()) return 0.0;

    double peak = equity.front();
    double max_drawdown = 0.0;
    for (double value : equity)
    {
        if (value > peak) peak = value;
        double drawdown = (peak > 0.0) ? (peak - value) / peak : 0.0;
        if (drawdown > max_drawdown) max_drawdown = drawdown;
    }
    return max_drawdown;
}

double compute_inventory_exposure(int64_t position)
{
    return static_cast<double>(std::llabs(position));
}
}  // namespace metrics
