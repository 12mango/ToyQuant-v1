#include "backtest/performance.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace metrics
{
namespace
{
double mean_of(const std::vector<double>& values)
{
    if (values.empty()) return 0.0;
    double sum = 0.0;
    for (double value : values) sum += value;
    return sum / static_cast<double>(values.size());
}

// The sample standard deviation, because the series is a sample of the run's own returns rather than
// the whole population of a process. One value has no spread to measure, so it reports zero.
double sample_stddev(const std::vector<double>& values)
{
    if (values.size() < 2) return 0.0;
    const double mean = mean_of(values);
    double squared = 0.0;
    for (double value : values) squared += (value - mean) * (value - mean);
    return std::sqrt(squared / static_cast<double>(values.size() - 1));
}
}  // namespace

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

std::vector<double> compute_returns(const std::vector<double>& equity)
{
    std::vector<double> returns;
    if (equity.size() < 2) return returns;
    returns.reserve(equity.size() - 1);
    for (std::size_t index = 1; index < equity.size(); ++index)
    {
        const double previous = equity[index - 1];
        returns.push_back(previous == 0.0 ? 0.0 : equity[index] / previous - 1.0);
    }
    return returns;
}

double compute_sharpe(const std::vector<double>& returns, double periods_per_year)
{
    if (returns.size() < 2 || periods_per_year <= 0.0) return 0.0;
    const double deviation = sample_stddev(returns);
    if (deviation <= 0.0) return 0.0;
    return mean_of(returns) / deviation * std::sqrt(periods_per_year);
}

double compute_sortino(const std::vector<double>& returns, double periods_per_year)
{
    if (returns.empty() || periods_per_year <= 0.0) return 0.0;
    // The downside deviation is taken about the zero target rather than about the mean, which is the
    // definition Sortino's own argument requires: a return below zero is the loss the ratio insures
    // against, whatever the average happened to be.
    double squared_downside = 0.0;
    for (double value : returns)
        if (value < 0.0) squared_downside += value * value;
    const double downside = std::sqrt(squared_downside / static_cast<double>(returns.size()));
    if (downside <= 0.0) return 0.0;
    return mean_of(returns) / downside * std::sqrt(periods_per_year);
}

double compute_annualized_volatility(const std::vector<double>& returns, double periods_per_year)
{
    if (returns.size() < 2 || periods_per_year <= 0.0) return 0.0;
    return sample_stddev(returns) * std::sqrt(periods_per_year);
}

double compute_annualized_return(const std::vector<double>& equity, double periods_per_year)
{
    if (equity.size() < 2 || periods_per_year <= 0.0) return 0.0;
    if (equity.front() <= 0.0 || equity.back() <= 0.0) return 0.0;
    const double intervals = static_cast<double>(equity.size() - 1);
    const double annualized =
        std::pow(equity.back() / equity.front(), periods_per_year / intervals) - 1.0;
    // A window short enough that its own exponent overflows has no rate to report; an infinity in a log
    // line is read as a broken measurement rather than as the boundary it is.
    return std::isfinite(annualized) ? annualized : 0.0;
}

double compute_calmar(double annualized_return, double max_drawdown)
{
    return max_drawdown > 0.0 ? annualized_return / max_drawdown : 0.0;
}

uint64_t compute_max_drawdown_duration(const std::vector<double>& equity,
                                       const std::vector<uint64_t>& timestamps_us)
{
    if (equity.empty() || equity.size() != timestamps_us.size()) return 0;

    double peak = equity.front();
    uint64_t peak_ts = timestamps_us.front();
    uint64_t longest = 0;
    bool in_drawdown = false;
    for (std::size_t index = 1; index < equity.size(); ++index)
    {
        // At or above the peak closes the drawdown that started there. The closing sample counts: the
        // question is how long the curve took to win the peak back, not how long it spent strictly
        // below it. A curve that never falls below its peak is never in a drawdown, so a flat series
        // reports zero rather than one interval per step.
        if (equity[index] >= peak)
        {
            if (in_drawdown) longest = std::max(longest, timestamps_us[index] - peak_ts);
            peak = equity[index];
            peak_ts = timestamps_us[index];
            in_drawdown = false;
            continue;
        }
        in_drawdown = true;
        const uint64_t elapsed =
            timestamps_us[index] > peak_ts ? timestamps_us[index] - peak_ts : 0;
        if (elapsed > longest) longest = elapsed;
    }
    return longest;
}
}  // namespace metrics
