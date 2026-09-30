// The checks in this test are the test: keep them enabled even when the build defines
// NDEBUG, which is the case for the RelWithDebInfo profiling preset.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "accounting/carry_ledger.h"
#include "common/instrument_spec.h"

#include <cassert>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
constexpr uint64_t kEightHours = 8ULL * 60ULL * 60ULL * 1000ULL;

FundingRateSeries series(const std::string& symbol,
                         std::initializer_list<std::pair<uint64_t, double>> points)
{
    FundingRateSeries rates(symbol);
    for (const auto& [ts, rate] : points) rates.add(ts, rate);
    return rates;
}

CarryConfig two_legs(FundingRateSeries a, bool long_a, FundingRateSeries b, bool long_b,
                     double notional_usd)
{
    CarryConfig config;
    config.leg_a = CarryLeg{.instrument = btc_usdt_spec(), .rates = std::move(a), .long_side = long_a};
    config.leg_b =
        CarryLeg{.instrument = okx_btc_swap_spec(), .rates = std::move(b), .long_side = long_b};
    config.notional_usd = notional_usd;
    return config;
}

bool threw(const std::function<void()>& action)
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
    // A carry that collects: Binance is long and pays 0.01% an interval, OKX is short and receives
    // 0.03%, so the position earns 2 USD an interval on 10,000 USD a leg. The settlement at the window
    // start is not applied, because the interval it covers ended before the hedge existed: the span
    // 0..16 h therefore holds two settlements per leg.
    {
        const auto summary = run_carry(two_legs(
            series("BTCUSDT", {{0, 0.0001}, {kEightHours, 0.0001}, {2 * kEightHours, 0.0001}}), true,
            series("BTC-USDT-SWAP", {{0, 0.0003}, {kEightHours, 0.0003}, {2 * kEightHours, 0.0003}}),
            false, 10000.0));

        assert(summary.leg_a_symbol == "BTCUSDT" && summary.leg_b_symbol == "BTC-USDT-SWAP");
        assert(summary.notional_usd == 10000.0);
        // The two legs together carry 20,000 USD and the account is exposed to none of it.
        assert(std::abs(summary.gross_notional_usd - 20000.0) < 1e-9);
        assert(std::abs(summary.net_notional_usd) < 1e-9);
        assert(summary.window_hours == 16);
        assert(summary.settlements == 4);
        // The long leg pays 1 USD an interval and the short leg receives 3, twice each.
        assert(std::abs(summary.leg_a_funding_usd + 2.0) < 1e-9);
        assert(std::abs(summary.leg_b_funding_usd - 6.0) < 1e-9);
        assert(std::abs(summary.funding_usd - 4.0) < 1e-9);
        // Entry and exit are taker fills at the venues' published rates: 10000 * (0.0004 + 0.0005) * 2.
        assert(std::abs(summary.fees_usd - 18.0) < 1e-9);
        assert(std::abs(summary.net_usd + 14.0) < 1e-9);
        // The acceptance number, and the form of it that does not depend on the window.
        assert(std::abs(summary.annualized_carry_usd - 4.0 * 365.0 * 24.0 / 16.0) < 1e-9);
        assert(std::abs(summary.annualized_return - 0.219) < 1e-12);
        assert(std::abs(summary.to_fees - summary.annualized_carry_usd / 18.0) < 1e-12);
        assert(std::abs(summary.breakeven_hours - 72.0) < 1e-9);
    }

    // A carry that pays instead of collecting: the long leg is on the expensive side of both venues, so the
    // funding is a cost, the acceptance number is negative and there is no holding period that pays the fees
    // back. Reporting the sign rather than a magnitude is the point: this position loses to its own funding
    // before its fees are counted.
    {
        const auto summary = run_carry(two_legs(
            series("BTCUSDT", {{0, 0.0005}, {kEightHours, 0.0005}, {2 * kEightHours, 0.0005}}), true,
            series("BTC-USDT-SWAP", {{0, 0.0001}, {kEightHours, 0.0001}, {2 * kEightHours, 0.0001}}),
            false, 10000.0));
        assert(std::abs(summary.funding_usd + 8.0) < 1e-9);
        assert(summary.annualized_carry_usd < 0.0);
        assert(summary.to_fees < 0.0);
        assert(summary.breakeven_hours == 0.0);
    }

    // The window is what both histories cover, which is what makes the number comparable: the Binance series
    // starts eight hours before the OKX one, so the carry runs for eight hours rather than sixteen, and the
    // settlements outside that span are consumed without being applied. Both legs enter the same rate here,
    // which is the hedge property: the funding cancels exactly and the fees are the whole cost.
    {
        const auto summary = run_carry(two_legs(
            series("BTCUSDT", {{0, 0.0002}, {kEightHours, 0.0002}, {2 * kEightHours, 0.0002}}), true,
            series("BTC-USDT-SWAP",
                   {{kEightHours, 0.0002}, {2 * kEightHours, 0.0002}, {3 * kEightHours, 0.0002}}),
            false, 10000.0));
        assert(summary.window_hours == 8);
        assert(summary.settlements == 2);
        assert(std::abs(summary.leg_a_funding_usd + 2.0) < 1e-9);
        assert(std::abs(summary.leg_b_funding_usd - 2.0) < 1e-9);
        assert(std::abs(summary.funding_usd) < 1e-12);
        assert(std::abs(summary.to_fees) < 1e-12);
        assert(std::abs(summary.net_usd + summary.fees_usd) < 1e-12);
    }

    // A carry that cannot be run is refused in words that name the reason, and running the same config twice
    // gives the same answer because run_carry takes its own copy of the two cursors.
    {
        const CarryConfig good = two_legs(
            series("BTCUSDT", {{0, 0.0001}, {kEightHours, 0.0001}}), true,
            series("BTC-USDT-SWAP", {{0, 0.0003}, {kEightHours, 0.0003}}), false, 10000.0);
        const CarrySummary first = run_carry(good);
        const CarrySummary second = run_carry(good);
        assert(first.to_log_string() == second.to_log_string());
        assert(std::abs(first.breakeven_hours - second.breakeven_hours) < 1e-12);

        // One instrument on both legs would net into a single position, which is not a hedge.
        {
            CarryConfig same = good;
            same.leg_b.instrument = btc_usdt_spec();
            assert(threw([&] { run_carry(same); }));
        }
        // A leg with no settlement has no carry to measure.
        {
            CarryConfig missing = good;
            missing.leg_b.rates = FundingRateSeries("BTC-USDT-SWAP");
            assert(threw([&] { run_carry(missing); }));
        }
        // A position of nothing, or of a negative notional, is not a position.
        {
            CarryConfig zero = good;
            zero.notional_usd = 0.0;
            assert(threw([&] { run_carry(zero); }));
            zero.notional_usd = -100.0;
            assert(threw([&] { run_carry(zero); }));
        }
        // Two histories that never overlap describe two different windows, so no position can be measured
        // against both.
        {
            CarryConfig disjoint = two_legs(
                series("BTCUSDT", {{0, 0.0001}, {kEightHours, 0.0001}}), true,
                series("BTC-USDT-SWAP", {{3 * kEightHours, 0.0003}, {4 * kEightHours, 0.0003}}), false,
                10000.0);
            assert(threw([&] { run_carry(disjoint); }));
        }
    }

    return 0;
}
