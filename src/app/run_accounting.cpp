#include "app/run_accounting.h"

#include <algorithm>

#include "backtest/performance.h"
#include "common/instrument_spec.h"
#include "common/types.h"

void RunAccounting::apply_due_funding(uint64_t ts, Portfolio& portfolio, double last_mid)
{
    if (funding_rates_ == nullptr) return;
    FundingRatePoint point;
    while (funding_rates_->next_settlement(ts, point))
    {
        ++funding_settlements_;
        // A settlement before the first price is applied to an account that holds nothing yet, so there is
        // no notional to charge and nothing to do; the settlement is still consumed, because the replay has
        // passed it and a later event must not apply it again.
        if (last_mid <= 0.0) continue;
        // The published rate is what longs pay; the accounting takes the rate a long earns on the notional,
        // so the sign is flipped once here and each position's own side then decides who pays whom.
        funding_flow_usd_ +=
            portfolio.accrue_interval_flow(funding_rates_->symbol(), -point.rate, last_mid);
    }
}

void RunAccounting::note_fill(Side side, double price, uint64_t quantity, uint64_t quote_cycle,
                              int bucket)
{
    pending_markouts_.push_back({side, price, quantity, quote_cycle, bucket});
}

void RunAccounting::note_market_print(Side passive_side, double price, uint64_t quantity,
                                      uint64_t quote_cycle, int bucket)
{
    pending_prints_.push_back(PassivePrint{.side = passive_side,
                                           .price = price,
                                           .quantity = quantity,
                                           .start_cycle = quote_cycle,
                                           .bucket = bucket});
}

void RunAccounting::drain_mature_markouts(uint64_t quote_cycle, double last_mid, double tick_size,
                                          double unit_notional_usd, double multiplier,
                                          uint64_t quantity_scale, ExecutionQualityMetrics& quality)
{
    // The observations are queued in cycle order, so the front is the oldest and the loop stops at the
    // first one whose horizon has not closed yet. Same five cycles on every path, and the arithmetic is
    // kept in the order it had when this was written twice, because the stdout hash is exact.
    while (!pending_markouts_.empty() && quote_cycle >= pending_markouts_.front().start_cycle + 5)
    {
        const auto observation = pending_markouts_.front();
        pending_markouts_.pop_front();
        const double markout = observation.side == Side::Buy ? observation.price - last_mid
                                                             : last_mid - observation.price;
        quality.adverse_selection += markout;
        quality.markout_quantity += static_cast<double>(observation.quantity);
        quality.markout_ticks_quantity +=
            markout / std::max(tick_size, PRICE_TICK_SIZE) * static_cast<double>(observation.quantity);
        const double unit_value_at_mark = unit_notional_at(unit_notional_usd, multiplier, last_mid);
        quality.markout_usd +=
            static_cast<double>(observation.quantity) / static_cast<double>(quantity_scale) *
            unit_value_at_mark * markout / std::max(last_mid, 1e-9);
        ++quality.markout_count;
        const std::size_t bucket =
            observation.bucket >= 0 ? static_cast<std::size_t>(observation.bucket) : 0;
        quality.markout_ticks_quantity_by_bucket[bucket] +=
            markout / std::max(tick_size, PRICE_TICK_SIZE) * static_cast<double>(observation.quantity);
        quality.markout_quantity_by_bucket[bucket] += static_cast<double>(observation.quantity);
        ++quality.markout_count_by_bucket[bucket];
    }
}

void RunAccounting::drain_mature_prints(uint64_t quote_cycle, double last_mid, double tick_size)
{
    // The market's own prints, resolved by the same five cycle horizon as our fills.
    while (!pending_prints_.empty() && quote_cycle >= pending_prints_.front().start_cycle + 5)
    {
        const PassivePrint print = pending_prints_.front();
        pending_prints_.pop_front();
        const double print_markout =
            print.side == Side::Buy ? last_mid - print.price : print.price - last_mid;
        const std::size_t print_bucket = print.bucket < 0 ? 0 : static_cast<std::size_t>(print.bucket);
        print_markout_ticks_quantity_[print_bucket] +=
            print_markout / std::max(tick_size, PRICE_TICK_SIZE) * static_cast<double>(print.quantity);
        print_markout_quantity_[print_bucket] += static_cast<double>(print.quantity);
        ++print_samples_;
    }
}

void RunAccounting::publish_markout_estimate(ExecutionQualityMetrics& quality, Strategy& strategy)
{
    // Our own fills are what the gate has to be built on. The venue's prints were tried first as a proxy,
    // and on the mainline window the two disagree in sign in the far bucket, which is recorded in
    // docs/PERFORMANCE_HISTORY.md: a print at a distance is not the same event as a quote at that distance
    // being hit, so the measured cost of the second has to come from the second.
    markout_estimate_.buy_far_ticks = markout_bucket_ticks(quality, 0);
    markout_estimate_.buy_near_ticks = markout_bucket_ticks(quality, 1);
    markout_estimate_.sell_far_ticks = markout_bucket_ticks(quality, 2);
    markout_estimate_.sell_near_ticks = markout_bucket_ticks(quality, 3);
    markout_estimate_.samples = quality.markout_count;
    for (std::size_t bucket = 0; bucket < print_markout_average_ticks_.size(); ++bucket)
        print_markout_average_ticks_[bucket] = print_markout_quantity_[bucket] > 0.0
                                                   ? print_markout_ticks_quantity_[bucket] /
                                                         print_markout_quantity_[bucket]
                                                   : 0.0;
    strategy.on_markout_estimate(markout_estimate_);
}

PnlSummary RunAccounting::pnl_summary(const Portfolio& portfolio, double maker_notional_usd,
                                      uint64_t trade_reports)
{
    const auto metrics = portfolio.metrics();
    PnlSummary pnl;
    pnl.closed_pnl_usd = metrics.realized_pnl;
    pnl.net_pnl_usd = metrics.realized_pnl + metrics.unrealized_pnl;
    pnl.gross_closed_pnl_usd = pnl.closed_pnl_usd + metrics.fees_paid;
    pnl.gross_pnl_usd = pnl.net_pnl_usd + metrics.fees_paid;
    pnl.maker_notional_usd = maker_notional_usd;
    if (trade_reports > 0)
    {
        const double fills = static_cast<double>(trade_reports);
        pnl.net_per_fill_usd = pnl.net_pnl_usd / fills;
        pnl.gross_closed_per_fill_usd = pnl.gross_closed_pnl_usd / fills;
    }
    // The maker rate that would have made the run exactly flat, on the notional its maker fills traded. A
    // negative answer is the statement the fee question is really asking about: no fee at all would still
    // lose money, and a rebate of that size would be needed to break even.
    if (maker_notional_usd > 0.0) pnl.breakeven_maker_rate = pnl.gross_pnl_usd / maker_notional_usd;
    return pnl;
}
