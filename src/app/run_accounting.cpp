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

void RunAccounting::set_parent_order(const ParentSpec& spec)
{
    parent_spec_ = spec;
    parent_requested_ =
        spec.side != Side::Unknown && spec.quantity > 0 && spec.horizon_us > 0;
}

void RunAccounting::start_parent(uint64_t ts, double decision_mid, double unit_notional_usd,
                                 double multiplier, uint64_t quantity_scale)
{
    if (!parent_requested_ || parent_started_ || decision_mid <= 0.0) return;
    parent_started_ = true;
    parent_start_ts_ = ts;
    parent_decision_mid_ = decision_mid;
    parent_unit_notional_usd_ = unit_notional_usd;
    parent_multiplier_ = multiplier;
    parent_quantity_scale_ = std::max<uint64_t>(1, quantity_scale);
    // The denominator of every basis point figure is the notional at the decision price, not the notional
    // actually traded: an order that never filled would otherwise have no denominator at all, which is exactly
    // the order whose shortfall matters most.
    const double units =
        static_cast<double>(parent_spec_.quantity) / static_cast<double>(parent_quantity_scale_);
    parent_decision_usd_ =
        units * unit_notional_at(parent_unit_notional_usd_, parent_multiplier_, parent_decision_mid_);
    // The benchmark integral starts at the decision, so the first observation is the baseline rather than a
    // segment of zero length.
    parent_last_mid_ts_ = ts;
    parent_last_mid_ = decision_mid;
}

void RunAccounting::observe_parent_mid(uint64_t ts, double mid)
{
    if (!parent_started_ || parent_finished_ || mid <= 0.0) return;
    if (ts > parent_last_mid_ts_)
    {
        // A step function of the midpoint: each observation holds until the next one, which is what the window
        // actually looked like between two events.
        parent_twap_area_ +=
            parent_last_mid_ * static_cast<double>(ts - parent_last_mid_ts_);
        parent_twap_time_us_ += ts - parent_last_mid_ts_;
    }
    parent_last_mid_ts_ = ts;
    parent_last_mid_ = mid;
}

void RunAccounting::note_parent_child()
{
    if (!parent_started_ || parent_finished_) return;
    ++parent_children_;
}

void RunAccounting::note_parent_fill(Side side, double price, uint64_t quantity, double fee)
{
    if (!parent_started_ || parent_finished_ || quantity == 0) return;
    parent_filled_quantity_ += quantity;
    parent_fees_usd_ += fee;
    const double units = static_cast<double>(quantity) / static_cast<double>(parent_quantity_scale_);
    parent_filled_units_ += units;
    parent_filled_units_price_ += units * price;
    // Signed as a cost, the same form the child arrival slippage uses: a buy above the decision price and a
    // sell below it are both money paid away.
    const double signed_move =
        side == Side::Buy ? price - parent_decision_mid_ : parent_decision_mid_ - price;
    parent_filled_usd_ +=
        units * unit_notional_at(parent_unit_notional_usd_, parent_multiplier_, parent_decision_mid_) *
        signed_move / parent_decision_mid_;
}

void RunAccounting::finish_parent(uint64_t ts, double horizon_mid)
{
    if (!parent_started_ || parent_finished_) return;
    // The last segment of the benchmark integral, from the last observation to the deadline. The right
    // endpoint is used because the horizon price is the one that closed the window.
    if (ts > parent_last_mid_ts_)
    {
        const double end_mid = horizon_mid > 0.0 ? horizon_mid : parent_last_mid_;
        parent_twap_area_ += end_mid * static_cast<double>(ts - parent_last_mid_ts_);
        parent_twap_time_us_ += ts - parent_last_mid_ts_;
    }
    parent_finished_ = true;
    parent_horizon_mid_ = horizon_mid;
    parent_span_seconds_ =
        ts > parent_start_ts_ ? static_cast<double>(ts - parent_start_ts_) / 1'000'000.0 : 0.0;

    const uint64_t unfilled = parent_spec_.quantity > parent_filled_quantity_
                                  ? parent_spec_.quantity - parent_filled_quantity_
                                  : 0;
    if (unfilled == 0 || horizon_mid <= 0.0 || parent_decision_mid_ <= 0.0) return;
    // What the quantity the parent never got would have cost at the horizon. This is the term an IOC run is
    // most likely to lose and the one a fill-only account cannot see.
    const double units = static_cast<double>(unfilled) / static_cast<double>(parent_quantity_scale_);
    const double signed_move = parent_spec_.side == Side::Buy ? horizon_mid - parent_decision_mid_
                                                              : parent_decision_mid_ - horizon_mid;
    parent_opportunity_usd_ +=
        units * unit_notional_at(parent_unit_notional_usd_, parent_multiplier_, parent_decision_mid_) *
        signed_move / parent_decision_mid_;
}

ExecutionAlgoSummary RunAccounting::parent_summary() const
{
    ExecutionAlgoSummary summary;
    summary.requested = parent_requested_;
    summary.complete = parent_finished_;
    summary.buy = parent_spec_.side == Side::Buy;
    summary.quantity = parent_spec_.quantity;
    summary.filled_quantity = parent_filled_quantity_;
    summary.child_orders = parent_children_;
    summary.decision_mid = parent_decision_mid_;
    summary.horizon_mid = parent_horizon_mid_;
    summary.span_seconds = parent_span_seconds_;
    summary.filled_ratio =
        parent_spec_.quantity > 0
            ? static_cast<double>(parent_filled_quantity_) / static_cast<double>(parent_spec_.quantity)
            : 0.0;
    // Basis points of the decision notional, with a positive number meaning money paid away.
    const double per_bp = parent_decision_usd_ > 0.0 ? 10000.0 / parent_decision_usd_ : 0.0;
    summary.is_filled_bps = parent_filled_usd_ * per_bp;
    summary.is_fees_bps = parent_fees_usd_ * per_bp;
    summary.is_opportunity_bps = parent_opportunity_usd_ * per_bp;
    summary.is_total_bps = summary.is_filled_bps + summary.is_fees_bps + summary.is_opportunity_bps;

    // The time-weighted benchmark over the same window, and the same three terms against it. This is the column
    // that separates a schedule from the market's drift: a parent that buys into a rising midpoint looks
    // expensive against its arrival price however it is worked, and only the average price it actually traded
    // at says whether the schedule helped.
    const double twap_mid = parent_twap_time_us_ > 0
                                ? parent_twap_area_ / static_cast<double>(parent_twap_time_us_)
                                : parent_decision_mid_;
    summary.twap_mid = twap_mid;
    if (parent_requested_ && twap_mid > 0.0)
    {
        const double sign = parent_spec_.side == Side::Buy ? 1.0 : -1.0;
        const double total_units =
            static_cast<double>(parent_spec_.quantity) / static_cast<double>(parent_quantity_scale_);
        const double unit_value =
            unit_notional_at(parent_unit_notional_usd_, parent_multiplier_, twap_mid);
        const double denominator = total_units * unit_value;
        if (denominator > 0.0)
        {
            // The parent's notional is the denominator for all three terms, so they stay additive with each
            // other and comparable with the arrival-price column.
            const double filled_usd = unit_value *
                                      sign * (parent_filled_units_price_ - twap_mid * parent_filled_units_) /
                                      twap_mid;
            summary.filled_vs_twap_bps = filled_usd / denominator * 10000.0;

            const double unfilled_units = total_units - parent_filled_units_;
            const double opportunity_usd = unfilled_units * unit_value * sign *
                                           (parent_horizon_mid_ - twap_mid) / twap_mid;
            summary.opportunity_vs_twap_bps = opportunity_usd / denominator * 10000.0;

            summary.is_vs_twap_bps = summary.filled_vs_twap_bps + summary.opportunity_vs_twap_bps +
                                     parent_fees_usd_ / denominator * 10000.0;
        }
    }
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
