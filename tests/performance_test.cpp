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

    // The return series is `E_t / E_{t-1} - 1`, one entry per interval, and a zero previous value has no
    // return to state rather than an infinite one.
    assert(metrics::compute_returns({}).empty());
    assert(metrics::compute_returns({100.0}).empty());
    {
        const auto returns = metrics::compute_returns({100.0, 120.0, 90.0, 150.0});
        assert(returns.size() == 3);
        assert(std::abs(returns[0] - 0.2) < 1e-12);
        assert(std::abs(returns[1] + 0.25) < 1e-12);
        assert(std::abs(returns[2] - (150.0 / 90.0 - 1.0)) < 1e-12);
    }
    assert(metrics::compute_returns({0.0, 100.0})[0] == 0.0);

    // A constant series has no dispersion, so every ratio and the volatility report zero instead of
    // dividing by it; the same holds for a series with no loss on the downside deviation.
    assert(metrics::compute_sharpe({}, 525600.0) == 0.0);
    assert(metrics::compute_sharpe({0.01}, 525600.0) == 0.0);
    assert(metrics::compute_sharpe({0.01, 0.01, 0.01}, 525600.0) == 0.0);
    // Two returns 0.01 apart around a mean of 0.015: the sample deviation is sqrt(0.00005), so the ratio
    // is 2.1213 and the factor of 4 in the square root turns it into sqrt(18).
    assert(std::abs(metrics::compute_sharpe({0.01, 0.02}, 4.0) - std::sqrt(18.0)) < 1e-12);
    assert(metrics::compute_sortino({0.01, 0.02}, 4.0) == 0.0);
    assert(std::abs(metrics::compute_sortino({0.02, -0.01}, 1.0) - std::sqrt(0.5)) < 1e-12);
    assert(std::abs(metrics::compute_annualized_volatility({0.01, 0.02}, 4.0) -
                    std::sqrt(0.0002)) < 1e-12);

    // The compounding annualization, not the arithmetic rate: ten percent over one interval at two
    // intervals a year is 21 percent, not 20.
    assert(std::abs(metrics::compute_annualized_return({100.0, 110.0}, 2.0) - 0.21) < 1e-12);
    assert(metrics::compute_annualized_return({100.0}, 2.0) == 0.0);
    assert(metrics::compute_annualized_return({100.0, 0.0}, 2.0) == 0.0);
    assert(metrics::compute_calmar(0.2, 0.1) == 2.0);
    assert(metrics::compute_calmar(0.2, 0.0) == 0.0);

    // A drawdown's duration is the time from the peak until the curve wins it back: 30 microseconds for
    // a curve that recovers at the fourth point, and the whole 20 for one that never does. A curve that
    // never falls below its peak and a pair of vectors that do not line up both report zero.
    assert(metrics::compute_max_drawdown_duration({100.0, 90.0, 95.0, 100.0, 105.0},
                                                  {0, 10, 20, 30, 40}) == 30);
    assert(metrics::compute_max_drawdown_duration({100.0, 100.0}, {0, 10}) == 0);
    assert(metrics::compute_max_drawdown_duration({100.0, 90.0, 80.0}, {0, 10, 20}) == 20);
    assert(metrics::compute_max_drawdown_duration({100.0}, {0}) == 0);
    assert(metrics::compute_max_drawdown_duration({100.0, 90.0}, {0}) == 0);
}