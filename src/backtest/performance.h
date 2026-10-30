#pragma once

#include <cstdint>
#include <vector>

namespace metrics
{
double compute_fill_rate(uint64_t submitted, uint64_t filled);
double compute_cancel_rate(uint64_t submitted, uint64_t cancelled);
double compute_max_drawdown(const std::vector<double>& equity);
double compute_inventory_exposure(int64_t position);

// The return series of an equity curve: `r_t = E_t / E_{t-1} - 1`, one entry per interval. A zero
// previous value has no return to state, so that interval is reported as zero rather than as an
// infinity, which is the same convention the drawdown below uses for a non-positive peak.
std::vector<double> compute_returns(const std::vector<double>& equity);

// The three risk-adjusted ratios, all annualized by the period count the caller states. `periods_per_year`
// is the number of samples a year holds at the sampling interval, not the number of samples in the
// window: an eight-minute replay has eight one-minute returns, and sqrt(8) would describe the window
// rather than the rate. A constant series has no dispersion and a series with no loss has no downside
// deviation, so each of those returns zero instead of a division by zero.
double compute_sharpe(const std::vector<double>& returns, double periods_per_year);
double compute_sortino(const std::vector<double>& returns, double periods_per_year);
double compute_annualized_volatility(const std::vector<double>& returns, double periods_per_year);

// `(E_last / E_0) ^ (periods_per_year / intervals) - 1`, the compounding form. It is deliberately not
// `mean(return) * periods_per_year`, which reports the arithmetic rate and overstates a volatile series.
// A window too short for its own exponent, or one that starts or ends at a non-positive equity, reports
// zero rather than an infinity.
double compute_annualized_return(const std::vector<double>& equity, double periods_per_year);

// Annualized return over the maximum drawdown. A run that never declined has no ratio to report.
double compute_calmar(double annualized_return, double max_drawdown);

// The longest time from an equity peak until the curve is back at that peak, in microseconds, which is
// the duration of the drawdown rather than its depth. A curve that ends below its last peak is still in
// that drawdown, so the peak whose recovery never came is measured to the end of the series. An empty
// curve, or two vectors of different lengths, reports zero: a duration without a matching timestamp is
// not a measurement.
uint64_t compute_max_drawdown_duration(const std::vector<double>& equity,
                                       const std::vector<uint64_t>& timestamps_us);
}  // namespace metrics
