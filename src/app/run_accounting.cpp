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
    while (!pending_markouts_.empty() &&
           quote_cycle >= pending_markouts_.front().start_cycle + markout_horizon_cycles_)
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
    while (!pending_prints_.empty() &&
           quote_cycle >= pending_prints_.front().start_cycle + markout_horizon_cycles_)
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

void RunAccounting::note_equity(uint64_t ts, double equity)
{
    // One point per bucket: a later observation in the bucket the last one belongs to replaces it, so
    // the stored point is the bucket's close and its timestamp is the moment that close was taken.
    if (!equity_curve_.empty() && ts / kEquityBucketUs == equity_curve_.back().ts / kEquityBucketUs)
    {
        equity_curve_.back() = {ts, equity};
        return;
    }
    equity_curve_.push_back({ts, equity});
}

PerformanceSummary RunAccounting::performance_summary() const
{
    PerformanceSummary summary;
    summary.samples = static_cast<uint64_t>(equity_curve_.size());
    if (equity_curve_.empty()) return summary;

    summary.span_seconds =
        static_cast<double>(equity_curve_.back().ts - equity_curve_.front().ts) / 1'000'000.0;
    // One sample per 60-second bucket, so a 24/7 instrument has 365 * 24 * 60 of them a year. The
    // factor is stated on the line because it, not the sample count, is what the ratios were scaled by:
    // a ten-minute window has ten buckets, and sqrt(10) would describe the window rather than the rate.
    summary.periods_per_year = 365.0 * 24.0 * 60.0;

    std::vector<double> equity;
    std::vector<uint64_t> timestamps;
    equity.reserve(equity_curve_.size());
    timestamps.reserve(equity_curve_.size());
    for (const auto& point : equity_curve_)
    {
        equity.push_back(point.equity);
        timestamps.push_back(point.ts);
    }

    const std::vector<double> returns = metrics::compute_returns(equity);
    summary.annualized_volatility =
        metrics::compute_annualized_volatility(returns, summary.periods_per_year);
    summary.sharpe = metrics::compute_sharpe(returns, summary.periods_per_year);
    summary.sortino = metrics::compute_sortino(returns, summary.periods_per_year);
    summary.max_drawdown = metrics::compute_max_drawdown(equity);
    summary.calmar = metrics::compute_calmar(
        metrics::compute_annualized_return(equity, summary.periods_per_year), summary.max_drawdown);
    const uint64_t drawdown_duration_us =
        metrics::compute_max_drawdown_duration(equity, timestamps);
    summary.max_drawdown_duration_seconds =
        static_cast<double>(drawdown_duration_us) / 1'000'000.0;
    return summary;
}

void RunAccounting::fill_round_trip_distribution(const std::vector<ClosedTrade>& trades,
                                                 std::optional<exchange::Side> side,
                                                 RoundTripSummary& summary)
{
    double net_total = 0.0;
    double win_sum = 0.0;
    double loss_sum = 0.0;
    double holding_seconds = 0.0;
    uint64_t wins = 0;
    uint64_t losses = 0;
    uint64_t count = 0;
    for (const auto& trade : trades)
    {
        // An empty filter is the whole book; otherwise only the positions opened on that side are counted.
        if (side.has_value() && trade.side != *side) continue;
        ++count;
        // The fee belongs to the fill, and the slice of it that closed this quantity was apportioned when
        // the trade was booked, so the net here is what the round trip contributed to realized PnL.
        const double net = trade.pnl - trade.fee;
        net_total += net;
        if (net > 0.0)
        {
            ++wins;
            win_sum += net;
        }
        else if (net < 0.0)
        {
            ++losses;
            loss_sum += net;
        }
        if (trade.exit_ts > trade.entry_ts)
            holding_seconds += static_cast<double>(trade.exit_ts - trade.entry_ts) / 1'000'000.0;
    }

    summary.trades = count;
    if (count == 0) return;
    const double trades_count = static_cast<double>(count);
    summary.win_rate = static_cast<double>(wins) / trades_count;
    summary.profit_factor = loss_sum < 0.0 ? -win_sum / loss_sum : 0.0;
    summary.expectancy_usd = net_total / trades_count;
    summary.average_win_usd = wins > 0 ? win_sum / static_cast<double>(wins) : 0.0;
    summary.average_loss_usd = losses > 0 ? loss_sum / static_cast<double>(losses) : 0.0;
    summary.average_holding_seconds = holding_seconds / trades_count;
}

RoundTripSummary RunAccounting::round_trip_summary(const std::vector<ClosedTrade>& trades,
                                                  double taker_notional_usd,
                                                  double total_notional_usd,
                                                  double starting_cash_usd)
{
    RoundTripSummary summary;
    summary.taker_notional_usd = taker_notional_usd;
    summary.total_notional_usd = total_notional_usd;
    summary.turnover = starting_cash_usd > 0.0 ? total_notional_usd / starting_cash_usd : 0.0;
    fill_round_trip_distribution(trades, std::nullopt, summary);
    return summary;
}

RoundTripSummary RunAccounting::side_round_trip_summary(const std::vector<ClosedTrade>& trades,
                                                       exchange::Side side)
{
    RoundTripSummary summary;
    fill_round_trip_distribution(trades, side, summary);
    return summary;
}

void RunAccounting::note_taker_fill(const TakerFillMeasurement& fill)
{
    ++taker_fills_;
    const double quantity = static_cast<double>(fill.quantity);
    taker_quantity_ += quantity;
    taker_slippage_ticks_quantity_ += fill.slippage_ticks * quantity;
    taker_slippage_bps_quantity_ += fill.slippage_bps * quantity;
    taker_slippage_usd_ += fill.slippage_usd;
    // One entry per horizon, in the fill's order, so each queue drains oldest first at its own horizon.
    for (auto& queue : pending_taker_edges_)
        queue.push_back({fill.side, fill.price, fill.quantity, fill.quote_cycle});
}

void RunAccounting::note_aggressive_order(const AggressiveOrder& order)
{
    ++aggressive_orders_;
    shortfall_filled_usd_ += order.filled_usd;
    shortfall_fees_usd_ += order.fees_usd;
    shortfall_decision_usd_ += order.decision_usd;
    const uint64_t unfilled = order.ordered_quantity > order.filled_quantity
                                  ? order.ordered_quantity - order.filled_quantity
                                  : 0;
    // Only the opportunity term has to wait: the other two are known when the order ends, and holding them
    // in the queue until the horizon would report them a few cycles late for no reason.
    pending_shortfalls_.push_back({.side = order.side,
                                   .decision_mid = order.decision_mid,
                                   .unfilled_quantity = unfilled,
                                   .start_cycle = order.start_cycle});
}

void RunAccounting::drain_taker_measurements(uint64_t quote_cycle, double last_mid, double tick_size,
                                             double unit_notional_usd, double multiplier,
                                             uint64_t quantity_scale)
{
    // The drift after each crossing fill, at each of the fixed horizons. The sign is the fill's own: a buy
    // that the market moved above has a positive edge, which is the opposite of the passive measurement's
    // "what came back after we were hit".
    for (std::size_t index = 0; index < pending_taker_edges_.size(); ++index)
    {
        auto& queue = pending_taker_edges_[index];
        const uint64_t horizon = kTakerEdgeHorizons[index];
        while (!queue.empty() && quote_cycle >= queue.front().start_cycle + horizon)
        {
            const TakerObservation observation = queue.front();
            queue.pop_front();
            const double edge = observation.side == Side::Buy ? last_mid - observation.price
                                                              : observation.price - last_mid;
            const double quantity = static_cast<double>(observation.quantity);
            taker_edge_ticks_quantity_[index] +=
                edge / std::max(tick_size, PRICE_TICK_SIZE) * quantity;
            taker_edge_quantity_[index] += quantity;
        }
    }

    // The unfilled remainder of each aggressive order: what the quantity it never got would have earned by
    // the horizon had it filled at the price the order decided at. Positive is a cost. An IOC that missed
    // the move is the case this exists for, and it is invisible in the filled quantities.
    while (!pending_shortfalls_.empty() &&
           quote_cycle >= pending_shortfalls_.front().start_cycle + markout_horizon_cycles_)
    {
        const PendingShortfall shortfall = pending_shortfalls_.front();
        pending_shortfalls_.pop_front();
        if (shortfall.unfilled_quantity == 0 || shortfall.decision_mid <= 0.0 || last_mid <= 0.0) continue;
        const double units = static_cast<double>(shortfall.unfilled_quantity) /
                             static_cast<double>(std::max<uint64_t>(1, quantity_scale));
        const double signed_move = shortfall.side == Side::Buy ? last_mid - shortfall.decision_mid
                                                               : shortfall.decision_mid - last_mid;
        const double unit_value = unit_notional_at(unit_notional_usd, multiplier, shortfall.decision_mid);
        shortfall_opportunity_usd_ += units * unit_value * signed_move / shortfall.decision_mid;
    }
}

TakerSummary RunAccounting::taker_summary() const
{
    TakerSummary summary;
    summary.fills = taker_fills_;
    summary.arrival_slippage_ticks =
        taker_quantity_ > 0.0 ? taker_slippage_ticks_quantity_ / taker_quantity_ : 0.0;
    summary.arrival_slippage_bps =
        taker_quantity_ > 0.0 ? taker_slippage_bps_quantity_ / taker_quantity_ : 0.0;
    summary.arrival_slippage_usd = taker_slippage_usd_;

    const auto edge_average = [this](std::size_t index) {
        return taker_edge_quantity_[index] > 0.0
                   ? taker_edge_ticks_quantity_[index] / taker_edge_quantity_[index]
                   : 0.0;
    };
    summary.edge_5_ticks = edge_average(0);
    summary.edge_20_ticks = edge_average(1);
    summary.edge_60_ticks = edge_average(2);

    summary.aggressive_orders = aggressive_orders_;
    summary.shortfall_filled_usd = shortfall_filled_usd_;
    summary.shortfall_fees_usd = shortfall_fees_usd_;
    summary.shortfall_opportunity_usd = shortfall_opportunity_usd_;
    summary.shortfall_usd =
        shortfall_filled_usd_ + shortfall_fees_usd_ + shortfall_opportunity_usd_;
    summary.shortfall_bps =
        shortfall_decision_usd_ > 0.0 ? summary.shortfall_usd / shortfall_decision_usd_ * 10000.0 : 0.0;
    return summary;
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
