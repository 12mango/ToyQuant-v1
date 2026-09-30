#include "accounting/carry_ledger.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "accounting/portfolio.h"

namespace
{
constexpr double kHoursPerYear = 365.0 * 24.0;
// A carry leg is a USD notional, so one quantity unit is one dollar and the mark of a unit is the unit
// itself. That is what makes the fee and the funding both exact without a price feed: the venue charges
// its taker rate on the notional and the rate accrues on the same number. A fixed-contract position's
// notional drifts with the mark instead, which is a second order term in funding against fees and is not
// modelled here.
constexpr double kUsdUnitPrice = 1.0;

InstrumentSpec usd_notional_leg(const InstrumentSpec& venue)
{
    return InstrumentSpec{.exchange = venue.exchange,
                          .symbol = venue.symbol,
                          .tick_size = 1.0,
                          .quantity_scale = 1,
                          .unit_notional_usd = 1.0,
                          .lot_size = 1,
                          .min_order_quantity = 1,
                          .maker_fee_rate = venue.maker_fee_rate,
                          .taker_fee_rate = venue.taker_fee_rate};
}

// What a leg's marked value is worth, taken from the ledger rather than from the number the command
// stated: a mode that opened unequal legs would report it.
double signed_leg_value(const PortfolioPosition& position)
{
    return static_cast<double>(position.quantity) * kUsdUnitPrice;
}
}  // namespace

std::string CarrySummary::to_log_string() const
{
    std::ostringstream stream;
    stream << "[CARRY] carry_leg_a=" << leg_a_symbol << " carry_leg_b=" << leg_b_symbol
           << " carry_notional_usd=" << notional_usd
           << " carry_gross_notional_usd=" << gross_notional_usd
           << " carry_net_notional_usd=" << net_notional_usd
           << " carry_window_hours=" << window_hours << " carry_settlements=" << settlements
           << " carry_leg_a_funding_usd=" << leg_a_funding_usd
           << " carry_leg_b_funding_usd=" << leg_b_funding_usd
           << " carry_funding_usd=" << funding_usd << " carry_fees_usd=" << fees_usd
           << " carry_net_usd=" << net_usd
           << " carry_annualized_carry_usd=" << annualized_carry_usd
           << " carry_annualized_return=" << annualized_return
           << " carry_to_fees=" << to_fees << " carry_breakeven_hours=" << breakeven_hours;
    return stream.str();
}

CarrySummary run_carry(CarryConfig config)
{
    const InstrumentSpec leg_a_venue = config.leg_a.instrument;
    const InstrumentSpec leg_b_venue = config.leg_b.instrument;
    if (leg_a_venue.symbol.empty() || leg_b_venue.symbol.empty())
        throw std::invalid_argument("a carry needs a symbol on both legs");
    if (leg_a_venue.symbol == leg_b_venue.symbol)
        throw std::invalid_argument("a carry needs two instruments: " + leg_a_venue.symbol +
                                    " on both legs would net into one position, not a hedge");
    if (!(config.notional_usd > 0.0)) throw std::invalid_argument("a carry needs a positive notional");
    if (config.leg_a.rates.empty() || config.leg_b.rates.empty())
        throw std::invalid_argument("a carry needs a funding history on both legs");

    // The window is the span the two histories share. A settlement before the other leg's history begins
    // cannot be part of a position measured against both, and applying it would credit the carry with time
    // the hedge did not exist.
    const uint64_t window_start = std::max(config.leg_a.rates[0].ts, config.leg_b.rates[0].ts);
    const uint64_t window_end =
        std::min(config.leg_a.rates[config.leg_a.rates.size() - 1].ts,
                 config.leg_b.rates[config.leg_b.rates.size() - 1].ts);
    if (window_end <= window_start)
        throw std::invalid_argument("the two funding histories do not overlap: " +
                                    std::to_string(window_start) + " >= " + std::to_string(window_end));

    const uint64_t quantity = static_cast<uint64_t>(std::llround(config.notional_usd));
    // The account holds one leg's notional: the long leg spends it and the short leg brings it back, so a
    // hedged position of this size needs no more collateral than its own notional.
    Portfolio portfolio(1, config.notional_usd, 1.0);
    uint64_t order_id = 0;
    const auto fill = [&](const InstrumentSpec& leg, exchange::Side side, uint64_t ts)
    {
        const double fee = fee_on_fill(leg.taker_fee_rate, leg.unit_notional_usd, kUsdUnitPrice,
                                       quantity, leg.quantity_scale);
        portfolio.apply(ExecutionReport{.order_id = ++order_id,
                                        .side = side,
                                        .exec_type = ExecType::Trade,
                                        .symbol = leg.symbol,
                                        .price = kUsdUnitPrice,
                                        .quantity = quantity,
                                        .ts = ts,
                                        .owner = "Carry",
                                        .liquidity_role = LiquidityRole::Taker,
                                        .fee = fee});
    };

    const InstrumentSpec leg_a = usd_notional_leg(leg_a_venue);
    const InstrumentSpec leg_b = usd_notional_leg(leg_b_venue);
    const exchange::Side side_a = config.leg_a.long_side ? exchange::Side::Buy : exchange::Side::Sell;
    const exchange::Side side_b = config.leg_b.long_side ? exchange::Side::Buy : exchange::Side::Sell;
    fill(leg_a, side_a, window_start);
    fill(leg_b, side_b, window_start);

    // The two legs' exposure, read from the ledger while both are open: their signed sum is what the account
    // carries, and their absolute sum is what a reader might mistake for it. A hedge adds the whole of one
    // leg to the second number and nothing to the first, which is the point of holding both.
    double net_notional_usd = 0.0;
    double gross_notional_usd = 0.0;
    for (const auto& [symbol, position] : portfolio.positions())
    {
        const double value = signed_leg_value(position);
        net_notional_usd += value;
        gross_notional_usd += std::abs(value);
    }

    // One settlement at a time, each on the leg it belongs to at that leg's own mark. The published rate is
    // what longs pay and the accounting takes the rate a long earns, so the sign is flipped once here and
    // each leg's own side then decides whether it pays or receives.
    const auto apply_leg = [&](FundingRateSeries& rates, const InstrumentSpec& leg)
    {
        double funding = 0.0;
        uint64_t applied = 0;
        FundingRatePoint point;
        // Settlements at or before the window start are consumed without being applied: the interval they
        // cover ended before the hedge existed.
        while (rates.next_settlement(window_start, point))
        {
        }
        while (rates.next_settlement(window_end, point))
        {
            funding += portfolio.accrue_interval_flow(leg.symbol, -point.rate, kUsdUnitPrice);
            ++applied;
        }
        return std::pair<double, uint64_t>{funding, applied};
    };
    const auto [leg_a_funding, leg_a_settlements] = apply_leg(config.leg_a.rates, leg_a);
    const auto [leg_b_funding, leg_b_settlements] = apply_leg(config.leg_b.rates, leg_b);

    fill(leg_a, side_a == exchange::Side::Buy ? exchange::Side::Sell : exchange::Side::Buy, window_end);
    fill(leg_b, side_b == exchange::Side::Buy ? exchange::Side::Sell : exchange::Side::Buy, window_end);

    CarrySummary summary;
    summary.leg_a_symbol = leg_a.symbol;
    summary.leg_b_symbol = leg_b.symbol;
    summary.notional_usd = config.notional_usd;
    summary.gross_notional_usd = gross_notional_usd;
    summary.net_notional_usd = std::abs(net_notional_usd);
    summary.window_hours = (window_end - window_start) / 3600000;
    summary.settlements = leg_a_settlements + leg_b_settlements;
    summary.leg_a_funding_usd = leg_a_funding;
    summary.leg_b_funding_usd = leg_b_funding;
    summary.funding_usd = leg_a_funding + leg_b_funding;
    summary.fees_usd = portfolio.metrics().fees_paid;
    summary.net_usd = summary.funding_usd - summary.fees_usd;
    if (summary.window_hours > 0)
    {
        summary.annualized_carry_usd =
            summary.funding_usd * kHoursPerYear / static_cast<double>(summary.window_hours);
        summary.annualized_return = summary.annualized_carry_usd / config.notional_usd;
        // The number the carry line is judged by: what the funding earns in a year against what the position
        // pays once to be opened and closed. Below one, the fees are the whole story.
        if (summary.fees_usd > 0.0) summary.to_fees = summary.annualized_carry_usd / summary.fees_usd;
        // The form of the same comparison that does not depend on the window: how long the position must be
        // held for the carry to have paid for itself.
        if (summary.funding_usd > 0.0)
            summary.breakeven_hours = summary.fees_usd * static_cast<double>(summary.window_hours) /
                                      summary.funding_usd;
    }
    return summary;
}
