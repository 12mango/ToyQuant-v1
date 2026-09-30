#include "market/funding_rate.h"

#include <charconv>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "market/line_reader.h"

namespace
{
// Milliseconds in a year of 365 days, the unit the annualized rate is expressed in. It is a fixed
// conversion rather than a calendar lookup, because a funding series is compared across venues and months,
// not settled against a date.
constexpr uint64_t kMsPerYear = 365ULL * 24ULL * 60ULL * 60ULL * 1000ULL;

constexpr std::string_view kOkxHeader = "instrument_name,funding_rate,funding_time";
constexpr std::string_view kBinanceHeader = "calc_time,funding_interval_hours,last_funding_rate";

// LineReader returns a view into its block with the line ending already removed, so a field split can
// run on the view as it arrives.
void split_fields(std::string_view row, std::vector<std::string_view>& fields)
{
    fields.clear();
    std::size_t begin = 0;
    while (true)
    {
        const std::size_t comma = row.find(',', begin);
        if (comma == std::string_view::npos)
        {
            fields.push_back(row.substr(begin));
            return;
        }
        fields.push_back(row.substr(begin, comma - begin));
        begin = comma + 1;
    }
}

uint64_t parse_uint(std::string_view value, std::string_view what)
{
    uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("invalid " + std::string(what) + ": " + std::string(value));
    return result;
}

double parse_rate(std::string_view value, std::string_view what)
{
    double result = 0.0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result,
                                        std::chars_format::general);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("invalid " + std::string(what) + ": " + std::string(value));
    return result;
}

std::string_view read_header(LineReader& reader, const std::string& path)
{
    std::string_view header;
    if (!reader.next_line(header))
        throw std::invalid_argument("funding rate file is empty: " + path);
    return header;
}
}  // namespace

void FundingRateSeries::add(uint64_t ts, double rate)
{
    if (!points_.empty() && ts < points_.back().ts)
        throw std::invalid_argument("funding rate points are out of order: " + std::to_string(ts) +
                                    " follows " + std::to_string(points_.back().ts));
    points_.push_back(FundingRatePoint{.ts = ts, .rate = rate});
}

double FundingRateSeries::sum_rate() const
{
    double total = 0.0;
    for (const auto& point : points_) total += point.rate;
    return total;
}

uint64_t FundingRateSeries::span_ms() const
{
    if (points_.size() < 2) return 0;
    return points_.back().ts - points_.front().ts;
}

double FundingRateSeries::annualized_rate() const
{
    const uint64_t span = span_ms();
    if (span == 0) return 0.0;
    return sum_rate() * static_cast<double>(kMsPerYear) / static_cast<double>(span);
}

bool FundingRateSeries::next_settlement(uint64_t ts, FundingRatePoint& out)
{
    if (consumed_ >= points_.size()) return false;
    const FundingRatePoint& next = points_[consumed_];
    if (next.ts > ts) return false;
    out = next;
    ++consumed_;
    return true;
}

std::string funding_rate_format_of(const std::string& path)
{
    LineReader probe(path);
    const std::string_view header = read_header(probe, path);
    if (header == kOkxHeader) return "okx";
    if (header == kBinanceHeader) return "binance";
    throw std::invalid_argument("unknown funding rate columns in " + path + ": " + std::string(header));
}

FundingRateSeries read_funding_rate_series(const std::string& path, const std::string& symbol)
{
    if (symbol.empty())
        throw std::invalid_argument("a funding rate series needs the instrument symbol it belongs to");
    const bool okx = funding_rate_format_of(path) == "okx";

    FundingRateSeries series(symbol);
    LineReader reader(path);
    read_header(reader, path);
    std::vector<std::string_view> fields;
    std::string_view row;
    while (reader.next_line(row))
    {
        if (row.empty()) continue;
        split_fields(row, fields);
        if (fields.size() < 3)
            throw std::invalid_argument("invalid funding rate row in " + path + ": " + std::string(row));
        if (okx)
        {
            // The daily file carries every swap on the venue, so the other instruments are not this series.
            if (fields[0] != symbol) continue;
            series.add(parse_uint(fields[2], "funding time"), parse_rate(fields[1], "funding rate"));
        }
        else
        {
            series.add(parse_uint(fields[0], "funding time"), parse_rate(fields[2], "funding rate"));
        }
    }
    return series;
}
