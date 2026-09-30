// The checks in this test are the test: keep them enabled even when the build defines
// NDEBUG, which is the case for the RelWithDebInfo profiling preset.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "accounting/portfolio.h"
#include "market/funding_rate.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>

namespace
{
// Milliseconds in a year, the conversion the annualized rate uses.
constexpr double kMsPerYear = 365.0 * 24.0 * 60.0 * 60.0 * 1000.0;
constexpr uint64_t kEightHours = 8ULL * 60ULL * 60ULL * 1000ULL;

std::string write_file(const std::string& name, const std::string& contents)
{
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    out.close();
    return path.string();
}

bool threw_invalid_argument(const std::function<void()>& action)
{
    try
    {
        action();
    }
    catch (const std::invalid_argument&)
    {
        return true;
    }
    return false;
}
}  // namespace

int main()
{
    // The OKX daily archive: one file for every swap on the venue, three columns, and a settlement for an
    // instrument this series is not about in the middle of it.
    const std::string okx_path = write_file(
        "toyquant_funding_okx.csv",
        "instrument_name,funding_rate,funding_time\n"
        "BTC-USDT-SWAP,0.0001,28800000\n"
        "BTC-USDT-SWAP,-0.0002,57600000\n"
        "ETH-USDT-SWAP,0.5,86400000\n"
        "BTC-USDT-SWAP,0.0003,86400000\n");
    assert(funding_rate_format_of(okx_path) == "okx");
    const FundingRateSeries okx = read_funding_rate_series(okx_path, "BTC-USDT-SWAP");
    assert(okx.symbol() == "BTC-USDT-SWAP");
    assert(okx.size() == 3);
    assert(okx[0].ts == 28800000 && std::abs(okx[0].rate - 0.0001) < 1e-15);
    assert(okx[1].ts == 57600000 && std::abs(okx[1].rate + 0.0002) < 1e-15);
    assert(okx[2].ts == 86400000 && std::abs(okx[2].rate - 0.0003) < 1e-15);
    // Three settlements 8 hours apart cover 16 hours, so the sum is what a long paid over that span and the
    // annualized rate is that sum continued over a year.
    assert(std::abs(okx.sum_rate() - 0.0002) < 1e-15);
    assert(okx.span_ms() == 2 * kEightHours);
    assert(std::abs(okx.annualized_rate() - 0.0002 * kMsPerYear / (2.0 * kEightHours)) < 1e-12);

    // The other instrument's row is not this series, and reading the file for a symbol it does not mention
    // gives an empty series rather than the venue's other rates.
    assert(read_funding_rate_series(okx_path, "ETH-USDT-SWAP").size() == 1);
    assert(read_funding_rate_series(okx_path, "NOT-LISTED").empty());

    // The Binance Vision monthly archive: one symbol per file, the interval declared per row, and the rate
    // in the third column.
    const std::string binance_path = write_file("toyquant_funding_binance.csv",
                                                "calc_time,funding_interval_hours,last_funding_rate\n"
                                                "28800000,8,0.001\n"
                                                "57600000,8,-0.002\n");
    assert(funding_rate_format_of(binance_path) == "binance");
    const FundingRateSeries binance = read_funding_rate_series(binance_path, "BTCUSDT");
    assert(binance.size() == 2);
    assert(binance[0].ts == 28800000 && std::abs(binance[0].rate - 0.001) < 1e-15);
    assert(binance[1].ts == 57600000 && std::abs(binance[1].rate + 0.002) < 1e-15);
    assert(std::abs(binance.sum_rate() + 0.001) < 1e-15);
    assert(binance.span_ms() == kEightHours);
    assert(std::abs(binance.annualized_rate() + 0.001 * kMsPerYear / static_cast<double>(kEightHours)) <
           1e-12);

    // Consumption is one settlement per call, in order, and never twice: the same clock reading must not
    // charge the same settlement again, which is what a replay re-entering the same millisecond would do.
    {
        FundingRatePoint point;
        FundingRateSeries series = read_funding_rate_series(okx_path, "BTC-USDT-SWAP");
        assert(!series.next_settlement(1, point));
        assert(series.next_settlement(28800000, point));
        assert(point.ts == 28800000);
        assert(!series.next_settlement(28800000, point));
        assert(series.consumed() == 1);
        assert(series.next_settlement(99999999, point));
        assert(point.ts == 57600000);
        assert(series.next_settlement(99999999, point));
        assert(point.ts == 86400000);
        assert(!series.next_settlement(99999999, point));
        assert(series.consumed() == 3);
    }

    // An empty series has no arithmetic and settles nothing, which is what a file read for an instrument it
    // does not mention produces.
    {
        FundingRateSeries none;
        FundingRatePoint point;
        assert(none.size() == 0 && none.span_ms() == 0);
        assert(std::abs(none.sum_rate()) < 1e-15);
        assert(std::abs(none.annualized_rate()) < 1e-15);
        assert(!none.next_settlement(1000, point));
    }

    // A backwards point is refused rather than sorted, because sorting it would hide a file that is not in
    // the order the reader assumes, and a span computed across the reordering would be wrong.
    {
        bool rejected = false;
        FundingRateSeries series;
        series.add(1000, 0.1);
        try
        {
            series.add(999, 0.1);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        assert(rejected);
        assert(series.size() == 1);
    }

    // Files that cannot be read are refused with a message rather than parsed into an empty series: an
    // unknown column set, an empty file, a row with a missing column, and a rate that is not a number.
    {
        const std::string unknown = write_file("toyquant_funding_unknown.csv", "a,b,c\n1,2,3\n");
        assert(threw_invalid_argument([&] { funding_rate_format_of(unknown); }));
        assert(threw_invalid_argument([&] { read_funding_rate_series(unknown, "BTCUSDT"); }));

        const std::string empty = write_file("toyquant_funding_empty.csv", "");
        assert(threw_invalid_argument([&] { read_funding_rate_series(empty, "BTCUSDT"); }));

        const std::string short_row = write_file("toyquant_funding_short.csv",
                                                 "calc_time,funding_interval_hours,last_funding_rate\n"
                                                 "28800000,8\n");
        assert(threw_invalid_argument([&] { read_funding_rate_series(short_row, "BTCUSDT"); }));

        const std::string bad_rate = write_file("toyquant_funding_bad_rate.csv",
                                                "calc_time,funding_interval_hours,last_funding_rate\n"
                                                "28800000,8,not-a-number\n");
        assert(threw_invalid_argument([&] { read_funding_rate_series(bad_rate, "BTCUSDT"); }));

        // A settlement list with one point has no span, so it has a sum but nothing to annualize.
        const std::string single = write_file("toyquant_funding_single.csv",
                                              "calc_time,funding_interval_hours,last_funding_rate\n"
                                              "28800000,8,0.0007\n");
        const FundingRateSeries one = read_funding_rate_series(single, "BTCUSDT");
        assert(one.size() == 1 && one.span_ms() == 0);
        assert(std::abs(one.sum_rate() - 0.0007) < 1e-15);
        assert(std::abs(one.annualized_rate()) < 1e-15);
    }

    // Where the series meets the ledger: the published rate is what longs pay, the accounting takes the rate
    // a long earns, so the sign is flipped once and each position's own sign decides who pays whom. A short
    // and a long of the same size enter the same published rate and their flows cancel exactly, which is the
    // property the two leg case will need.
    {
        constexpr double kPublishedRate = 0.0001;
        Portfolio long_leg(1, 1000.0, 10.0);
        long_leg.apply(ExecutionReport{.side = exchange::Side::Buy,
                                       .exec_type = ExecType::Trade,
                                       .symbol = "BTC-PERPETUAL",
                                       .price = 6000.0,
                                       .quantity = 2,
                                       .owner = "test",
                                       .liquidity_role = LiquidityRole::Maker,
                                       .fee = 0.0});
        long_leg.mark_to_market({{"BTC-PERPETUAL", 6000.0}});
        const double long_cash_before = long_leg.metrics().cash;
        // Two contracts at a 10 USD face value is 20 USD, so a 0.01% interval takes 0.002 USD from the long.
        const double long_flow = long_leg.accrue_interval_flow("BTC-PERPETUAL", -kPublishedRate, 6000.0);
        assert(std::abs(long_flow + 0.002) < 1e-12);
        assert(std::abs(long_leg.metrics().cash - (long_cash_before + long_flow)) < 1e-15);

        Portfolio short_leg(1, 1000.0, 10.0);
        short_leg.apply(ExecutionReport{.side = exchange::Side::Sell,
                                        .exec_type = ExecType::Trade,
                                        .symbol = "BTC-PERPETUAL",
                                        .price = 6000.0,
                                        .quantity = 2,
                                        .owner = "test",
                                        .liquidity_role = LiquidityRole::Maker,
                                        .fee = 0.0});
        short_leg.mark_to_market({{"BTC-PERPETUAL", 6000.0}});
        const double short_cash_before = short_leg.metrics().cash;
        const double short_flow =
            short_leg.accrue_interval_flow("BTC-PERPETUAL", -kPublishedRate, 6000.0);
        // The same published rate and the same call, with the opposite sign: the same instrument's funding
        // cancels across a hedge instead of being charged twice in one direction.
        assert(std::abs(short_flow + long_flow) < 1e-15);
        assert(std::abs(short_leg.metrics().cash - (short_cash_before + short_flow)) < 1e-15);
    }

    // The real archives, when this checkout has them. data/v2 is not tracked, so a fresh clone skips this
    // rather than failing: the shape is what the test asserts, and the arithmetic over these files is quoted
    // in a recorded block that tools/check_docs.py re-derives.
    {
        const std::filesystem::path root(PROJECT_ROOT_DIR);
        const auto month = root / "data/v2/BTCUSDT-fundingRate-2024-03.csv";
        if (std::filesystem::is_regular_file(month))
        {
            const FundingRateSeries series = read_funding_rate_series(month.string(), "BTCUSDT");
            // A 31 day month on an eight hour interval: 93 settlements and 92 gaps. The venue's settlement
            // clock carries a few milliseconds of jitter (several calc_time values end in ...000001), so the
            // interval is eight hours to within a second rather than exactly, and a strict equality here
            // would be a red test about the venue's clock instead of about this reader.
            assert(series.size() == 93);
            assert(std::abs(static_cast<double>(series.span_ms()) - 92.0 * kEightHours) < 2000.0);
            for (std::size_t index = 1; index < series.size(); ++index)
                assert(std::abs(static_cast<double>(series[index].ts - series[index - 1].ts) -
                                kEightHours) < 2000.0);
            // March 2024 was a month a long paid to hold: the settled rates sum positive and annualize to a
            // positive carry, which is the sign the accounting must not arrive at on its own.
            assert(series.sum_rate() > 0.0);
            assert(series.annualized_rate() > 0.0);
        }
        const auto day = root / "data/v2/okx_funding_2026-01-07_BTC-USDT-SWAP.csv";
        if (std::filesystem::is_regular_file(day))
        {
            // The daily file carries the whole venue, so three settlements for one instrument prove the
            // symbol filter along with the columns.
            const FundingRateSeries series = read_funding_rate_series(day.string(), "BTC-USDT-SWAP");
            assert(series.size() == 3);
            assert(series.span_ms() == 2 * kEightHours);
            assert(read_funding_rate_series(day.string(), "BTC-USDT").empty());
        }
    }

    return 0;
}
