#include "app/fill_accounting.h"

#include <algorithm>
#include <cmath>

#include "app/run_accounting.h"
#include "common/instrument_spec.h"

FillAccounting::FillAccounting(ExecutionQualityMetrics& quality, RunAccounting& accounting)
    : quality_(quality), accounting_(accounting)
{
}

void FillAccounting::apply(const ExecutionReport& report, const FillContext& context)
{
    // The notional a fill traded, which is what its fee was charged on. The breakeven fee rate needs the
    // maker half, and it cannot be recovered from the fee itself once the rate is zero, which is exactly
    // the case that has to be measured. The two halves are kept apart because they are different
    // businesses: one is quoted and one is crossed, and only their sum is turnover.
    const double fill_notional =
        fill_notional_usd(context.unit_notional_usd, context.multiplier, report.price,
                          report.executed_quantity(), context.quantity_scale);
    if (report.liquidity_role == LiquidityRole::Maker)
        maker_notional_usd_ += fill_notional;
    else if (report.liquidity_role == LiquidityRole::Taker)
        taker_notional_usd_ += fill_notional;

    const int64_t previous_position = position_;
    position_ += report.side == exchange::Side::Buy
                     ? static_cast<int64_t>(report.executed_quantity())
                     : -static_cast<int64_t>(report.executed_quantity());
    // The position only changes here, so this is where the peak and the sign changes belong. Sampling
    // the peak from a market view instead missed a fill that arrived as the last event, and the summary
    // then reported a maximum inventory of zero next to an open marked position.
    quality_.max_abs_inventory = std::max(quality_.max_abs_inventory, std::abs(position_));
    if (previous_position != 0 && position_ != 0 && (previous_position < 0) != (position_ < 0))
        ++quality_.inventory_sign_changes;

    // The fill consumes the reservation the position gate made for it. A malformed side releases the
    // sell reservation as the old report path did.
    release(report.side == exchange::Side::Buy ? Side::Buy : Side::Sell,
            report.executed_quantity(), true);

    // Marked exposure against the collateral the run started with. The documented model is cash only
    // and no margin, and nothing else in the simulator enforces or reports it.
    const double exposure =
        std::abs(static_cast<double>(position_)) / static_cast<double>(context.quantity_scale) *
        unit_notional_at(context.unit_notional_usd, context.multiplier, report.price);
    max_abs_exposure_usd_ = std::max(max_abs_exposure_usd_, exposure);
    if (context.starting_cash_usd > 0.0 && exposure > context.starting_cash_usd)
        ++exposure_over_collateral_fills_;

    const double unit_edge = report.side == exchange::Side::Buy ? context.last_mid - report.price
                                                                : report.price - context.last_mid;
    // The raw sum mixes prices and quantities, so it cannot be compared against the half-spread the
    // quote sat at, nor across runs whose fills have different sizes. The weighted sums behind it
    // produce a quantity-weighted average in ticks, which is the number a market maker would quote
    // a spread against.
    const auto executed = static_cast<double>(report.executed_quantity());
    quality_.captured_edge += unit_edge;
    quality_.captured_edge_quantity += executed;
    quality_.captured_edge_ticks_quantity +=
        unit_edge / std::max(context.tick_size, PRICE_TICK_SIZE) * executed;
    // The same edge in money. A price difference on an inverse contract moves the position by
    // quantity * face value * edge / price, so this is the figure that can be compared with fees_paid.
    const double unit_value_at_fill =
        unit_notional_at(context.unit_notional_usd, context.multiplier, report.price);
    quality_.captured_edge_usd +=
        executed / static_cast<double>(context.quantity_scale) * unit_value_at_fill * unit_edge /
        std::max(report.price, 1e-9);

    const bool at_least_a_tick_away =
        std::abs(report.price - context.last_mid) >= std::max(context.tick_size, PRICE_TICK_SIZE);
    const int markout_bucket =
        (report.side == exchange::Side::Buy ? 0 : 2) + (at_least_a_tick_away ? 0 : 1);
    accounting_.note_fill(report.side == exchange::Side::Buy ? Side::Buy : Side::Sell, report.price,
                          report.executed_quantity(), context.quote_cycle, markout_bucket);

    ++trade_reports_;
    trade_report_quantity_ += report.executed_quantity();
}

void FillAccounting::reserve(Side side, uint64_t quantity)
{
    if (quantity == 0) return;
    if (side == Side::Buy)
        pending_buy_quantity_ += static_cast<int64_t>(quantity);
    else if (side == Side::Sell)
        pending_sell_quantity_ += static_cast<int64_t>(quantity);
}

void FillAccounting::release(Side side, uint64_t quantity, bool sell_on_unknown)
{
    const auto released = static_cast<int64_t>(quantity);
    if (side == Side::Buy)
        pending_buy_quantity_ -= std::min(pending_buy_quantity_, released);
    else if (side == Side::Sell || sell_on_unknown)
        pending_sell_quantity_ -= std::min(pending_sell_quantity_, released);
}
