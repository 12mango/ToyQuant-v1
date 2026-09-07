#include "backtest/performance.h"

#include <cassert>
#include <cmath>
#include <vector>

int main()
{
    assert(metrics::compute_fill_rate(0, 10) == 0.0);
    assert(metrics::compute_fill_rate(200, 50) == 0.25);

    assert(metrics::compute_cancel_rate(0, 10) == 0.0);
    assert(metrics::compute_cancel_rate(20, 5) == 0.25);

    assert(metrics::compute_inventory_exposure(250) == 250.0);
    assert(metrics::compute_inventory_exposure(-250) == 250.0);

    assert(metrics::compute_max_drawdown({}) == 0.0);
    assert(std::abs(metrics::compute_max_drawdown({100.0, 120.0, 90.0, 150.0}) - 0.25) < 1e-12);
}