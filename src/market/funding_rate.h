#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// One funding settlement, as the venue published it. `rate` is the per-interval rate the longs pay, which
// is the direction both venues publish: a positive rate costs a long position and pays a short one, and
// the accounting takes the rate a long *earns*, so the sign is flipped once, where the series is applied.
struct FundingRatePoint
{
    uint64_t ts{0};    // milliseconds, the venue's settlement time
    double rate{0.0};  // per interval, longs pay when positive
};

// The funding history of one instrument, in settlement order, plus how far a replay has consumed it.
//
// It is an input but not a MarketEvent, because nothing about it arrives in the event stream: a settlement
// is a property of the clock, and the replay reaches it between two market events. The pipeline therefore
// holds the series and drains it as the event clock passes each settlement, instead of merging a point per
// eight hours into the feed and paying an allocation for it.
class FundingRateSeries
{
   public:
    FundingRateSeries() = default;
    explicit FundingRateSeries(std::string symbol) : symbol_(std::move(symbol)) {}

    const std::string& symbol() const
    {
        return symbol_;
    }

    bool empty() const
    {
        return points_.empty();
    }

    std::size_t size() const
    {
        return points_.size();
    }

    const FundingRatePoint& operator[](std::size_t index) const
    {
        return points_[index];
    }

    // Appends one settlement. A file is read in order, and a list that goes backwards in time would
    // annualize over a negative span, so an out-of-order point is refused rather than sorted: sorting it
    // would hide a file that is not in the order the reader assumes.
    void add(uint64_t ts, double rate);

    // The summed rates of the file: what a long holding one unit of notional paid over the covered span,
    // per unit of notional. Negative means the long was paid.
    double sum_rate() const;

    // Milliseconds between the first and the last settlement, which is the span the sum is spread over.
    uint64_t span_ms() const;

    // The file's summed rate continued over a year, which is the comparable form of carry: the sum is
    // spread over the span the file covers, so a month and a year of the same average rate annualize to
    // the same number. Only the settlement timestamps and the rates enter, so both file formats produce it
    // the same way, and the Binance file's declared interval column is a check on the timestamps rather
    // than an input here.
    double annualized_rate() const;

    // The next settlement not yet consumed, if it falls at or before `ts`. Consumption is explicit and
    // happens once per call, so a settlement is applied exactly once, and a caller that stops early leaves
    // the rest of the series readable.
    bool next_settlement(uint64_t ts, FundingRatePoint& out);

    std::size_t consumed() const
    {
        return consumed_;
    }

   private:
    std::string symbol_;
    std::vector<FundingRatePoint> points_;
    std::size_t consumed_{0};
};

// Which reader a funding file needs, decided from its own header rather than from its name:
//   * OKX daily archive: `instrument_name,funding_rate,funding_time`, every swap on the venue in one file
//   * Binance Vision monthly archive: `calc_time,funding_interval_hours,last_funding_rate`, one symbol
// Returns "okx" or "binance", and throws std::invalid_argument for anything else.
std::string funding_rate_format_of(const std::string& path);

// Reads one instrument's funding history. A plain or gzipped CSV, through the same LineReader every other
// reader uses. The symbol is required for the OKX file, which carries the whole venue; the Binance file is
// one symbol already and the argument then only labels the series.
FundingRateSeries read_funding_rate_series(const std::string& path, const std::string& symbol);
