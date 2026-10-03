#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

#include "backtest/performance.h"
#include "exchange/execution_report.h"

namespace
{
// Nearest-rank percentile over a copy. One entry per order, so a sort per call is cheaper than anything
// clever, and the rank is rounded to the nearest entry rather than interpolated because the values are
// ratios between two integers and inventing intermediate values would suggest a precision they do not have.
double percentile(std::vector<double> values, double fraction)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double position = fraction * static_cast<double>(values.size() - 1);
    const std::size_t index =
        std::min(values.size() - 1, static_cast<std::size_t>(position + 0.5));
    return values[index];
}

// Which bucket a filled order belongs in, by how many shares it filled in total. The bounds are where the
// two share schedules this repository has read stop charging a minimum per order: 100 shares is 0.35 at a
// 0.0035 share rate, and 200 shares is 1.00 at 0.005. Both thresholds therefore fall on a boundary, so a
// commission under either convention is computable exactly from the counts and the quantity sums.
std::size_t fill_size_bucket(uint64_t quantity)
{
    if (quantity <= 50) return 0;
    if (quantity <= 99) return 1;
    if (quantity <= 199) return 2;
    if (quantity <= 499) return 3;
    if (quantity <= 999) return 4;
    return 5;
}

}  // namespace

Pipeline::Pipeline(std::ofstream& orders_out, std::ofstream& trades_out, IOrderBook& order_book,
                   Strategy& strategy, IMatchingEngine& engine, Portfolio& portfolio,
                   double tick_size)
    : ledger_(orders_out, trades_out),
      order_book_(order_book),
      strategy_(strategy),
      engine_(engine),
      portfolio_(portfolio),
      tick_size_(tick_size)
{
    engine_.set_report_callback(
        [this](const ExecutionReport& report)
        {
            ledger_.trace_report(report, current_ts_, engine_);
            if (report.exec_type == ExecType::Resting)
            {
                const auto resting_audit = order_audit_.find(report.order_id);
                if (resting_audit != order_audit_.end())
                {
                    OrderAudit& audit = resting_audit->second;
                    audit.profiling = true;
                    audit.symbol = report.symbol;
                    audit.side = report.side == exchange::Side::Buy ? Side::Buy : Side::Sell;
                    audit.price = report.price;
                    // The queue this order joined behind. The engine has just created the level, and the
                    // level exists only while one of our orders rests in it, which is why both ends of the
                    // measurement are taken while the order is still there.
                    audit.queue_at_rest =
                        engine_.queue_ahead_at(audit.symbol, audit.side, audit.price);
                    audit.min_queue_ahead = audit.queue_at_rest;
                }
            }
            if (report.owner == "MarketMaker") portfolio_.apply(report);
            // The notional a maker fill traded, which is what its fee was charged on. The breakeven fee rate
            // needs it, and it cannot be recovered from the fee itself once the rate is zero, which is
            // exactly the case that has to be measured.
            if (report.owner == "MarketMaker" && report.exec_type == ExecType::Trade &&
                report.liquidity_role == LiquidityRole::Maker)
                maker_notional_usd_ +=
                    fill_notional_usd(unit_notional_usd_, multiplier_, report.price, report.executed_quantity(),
                                      quantity_scale_);
            if (report.owner == "MarketMaker" && report.exec_type == ExecType::Trade)
            {
                const int64_t previous_position = position_;
                position_ += report.side == exchange::Side::Buy
                                 ? static_cast<int64_t>(report.executed_quantity())
                                 : -static_cast<int64_t>(report.executed_quantity());
                // The position only changes here, so this is where the peak and the sign changes
                // belong. Sampling the peak from a market view instead missed a fill that arrived as
                // the last event, and the summary then reported a maximum inventory of zero next to an
                // open marked position. The sign changes were never counted at all.
                execution_quality_.max_abs_inventory =
                    std::max(execution_quality_.max_abs_inventory, std::abs(position_));
                if (previous_position != 0 && position_ != 0 &&
                    (previous_position < 0) != (position_ < 0))
                    ++execution_quality_.inventory_sign_changes;
                // The order is no longer resting, so give the gate its reserved quantity back.
                if (report.side == exchange::Side::Buy)
                    pending_buy_quantity_ -=
                        std::min(pending_buy_quantity_,
                                 static_cast<int64_t>(report.executed_quantity()));
                else
                    pending_sell_quantity_ -=
                        std::min(pending_sell_quantity_,
                                 static_cast<int64_t>(report.executed_quantity()));
                // Marked exposure against the collateral the run started with. The documented model is
                // cash only and no margin, and nothing else in the simulator enforces or reports it:
                // a strategy without an inventory limit reached a cash balance of -120,940 USD on this
                // feed while equity still printed +29%, which no component flagged.
                const double exposure =
                    std::abs(static_cast<double>(position_)) /
                    static_cast<double>(quantity_scale_) *
                    unit_notional_at(unit_notional_usd_, multiplier_, report.price);
                max_abs_exposure_usd_ = std::max(max_abs_exposure_usd_, exposure);
                if (starting_cash_usd_ > 0.0 && exposure > starting_cash_usd_)
                    ++exposure_over_collateral_fills_;
                const double unit_edge = report.side == exchange::Side::Buy
                                             ? last_mid_ - report.price
                                             : report.price - last_mid_;
                // The raw sum mixes prices and quantities, so it cannot be compared against the
                // half-spread the quote sat at, nor across runs whose fills have different sizes.
                // The weighted sums behind it produce a quantity-weighted average in ticks, which is
                // the number a market maker would actually quote a spread against.
                const auto executed = static_cast<double>(report.executed_quantity());
                execution_quality_.captured_edge += unit_edge;
                execution_quality_.captured_edge_quantity += executed;
                execution_quality_.captured_edge_ticks_quantity +=
                    unit_edge / std::max(tick_size_, PRICE_TICK_SIZE) * executed;
                // The same edge in money. A price difference on an inverse contract moves the position
                // by quantity * face value * edge / price, so this is the figure that can be compared
                // with fees_paid. For a spot instrument the face value is zero, the price is the unit
                // value, and the expression reduces to quantity * edge.
                const double unit_value_at_fill = unit_notional_at(unit_notional_usd_, multiplier_, report.price);
                execution_quality_.captured_edge_usd +=
                    executed / static_cast<double>(quantity_scale_) * unit_value_at_fill *
                    unit_edge / std::max(report.price, 1e-9);
                // The condition is read here, at the fill: the side, and whether the fill price was at least a
                // tick away from the midpoint then. The first attempt used the position, and it showed that with
                // this sizing every fill starts from flat, so the second dimension has to be one that varies and
                // that a quote decision could act on.
                const bool at_least_a_tick_away =
                    std::abs(report.price - last_mid_) >= std::max(tick_size_, PRICE_TICK_SIZE);
                const int markout_bucket =
                    (report.side == exchange::Side::Buy ? 0 : 2) + (at_least_a_tick_away ? 0 : 1);
                pending_markouts_.push_back(
                    {report.side == exchange::Side::Buy ? Side::Buy : Side::Sell, report.price,
                     report.executed_quantity(), quote_cycle_, markout_bucket});
            }
            if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
            {
                const auto start = order_start_cycles_.find(report.order_id);
                if (start != order_start_cycles_.end())
                {
                    const uint64_t lifetime = quote_cycle_ >= start->second
                                                  ? quote_cycle_ - start->second
                                                  : 0;
                    execution_quality_.total_quote_lifetime += lifetime;
                    execution_quality_.max_quote_lifetime =
                        std::max(execution_quality_.max_quote_lifetime, lifetime);
                    order_start_cycles_.erase(start);
                }
            }
            strategy_.on_order_update(report);
            // The strategy keeps its own position from the same reports. A divergence is silent drift
            // in every quote it sizes, so it is counted here rather than assumed away.
            if (report.owner == "MarketMaker" && strategy_.net_position() != position_)
                ++strategy_position_mismatches_;
            if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
            {
                const auto remaining = static_cast<int64_t>(report.remaining_quantity());
                if (report.side == exchange::Side::Buy)
                    pending_buy_quantity_ -= std::min(pending_buy_quantity_, remaining);
                else
                    pending_sell_quantity_ -= std::min(pending_sell_quantity_, remaining);
                pending_cancel_orders_.erase(report.order_id);
            }
            auto audit = order_audit_.find(report.order_id);
            if (audit != order_audit_.end())
            {
                if (report.exec_type == ExecType::Trade) audit->second.filled = true;
                if (report.exec_type == ExecType::Trade)
                    audit->second.filled_quantity += report.executed_quantity();
                if (report.exec_type == ExecType::Trade && commission_per_unit_ > 0.0)
                {
                    // The broker's own commission, accumulated per order so that the floor can be applied to
                    // the order's total when it ends. The venue's rate is not part of it: a broker's floor is
                    // a floor on its commission.
                    const double units =
                        quantity_scale_ > 0 ? static_cast<double>(report.executed_quantity()) /
                                                  static_cast<double>(quantity_scale_)
                                            : static_cast<double>(report.executed_quantity());
                    audit->second.commission += units * commission_per_unit_;
                }
                if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
                {
                    const uint64_t lifetime = quote_cycle_ >= audit->second.start_cycle
                                                  ? quote_cycle_ - audit->second.start_cycle
                                                  : 0;
                    execution_quality_.total_order_lifetime_cycles += lifetime;
                    if (audit->second.filled && lifetime > longest_wait_filled_cycles_)
                    {
                        longest_wait_filled_cycles_ = lifetime;
                        longest_wait_filled_order_id_ = report.order_id;
                    }
                    if (report.exec_type == ExecType::Cancelled && audit->second.filled)
                    {
                        ++execution_quality_.cancelled_after_fill_orders;
                    }
                    else if (report.exec_type == ExecType::Cancelled)
                    {
                        ++execution_quality_.cancelled_before_fill_orders;
                    }
                    if (report.exec_type == ExecType::Filled)
                        ++execution_quality_.audited_filled_orders;
                    else
                        ++execution_quality_.audited_cancelled_orders;
                    if (audit->second.profiling && audit->second.queue_at_rest > 0)
                    {
                        const double at_rest = static_cast<double>(audit->second.queue_at_rest);
                        queue_min_fractions_.push_back(
                            static_cast<double>(audit->second.min_queue_ahead) / at_rest);
                        queue_end_fractions_.push_back(
                            static_cast<double>(audit->second.queue_at_end) / at_rest);
                        ++queue_profile_orders_;
                        if (audit->second.min_queue_ahead == 0) ++queue_zero_orders_;
                    }
                    // The order instance, not the fill, is what a broker's minimum per order is charged
                    // on, so it is counted once here, when the order ends, and bucketed by how much it
                    // filled. A modification or a cancel and replace makes a new instance, which is why
                    // this counter and the fill count are not the same number.
                    if (audit->second.filled)
                    {
                        ++execution_quality_.orders_that_filled;
                        const std::size_t bucket = fill_size_bucket(audit->second.filled_quantity);
                        ++execution_quality_.filled_order_count_by_size[bucket];
                        execution_quality_.filled_order_quantity_by_size[bucket] +=
                            audit->second.filled_quantity;
                        // The floor, which is why this counter exists: an order that filled less than the
                        // floor's worth of shares owes the difference, and the order ending is the only
                        // moment the total is known.
                        const double shortfall =
                            min_commission_shortfall(min_commission_per_order_, audit->second.commission);
                        if (shortfall > 0.0)
                        {
                            portfolio_.charge_fee(shortfall);
                            ++execution_quality_.min_commission_orders;
                            execution_quality_.min_commission_paid += shortfall;
                        }
                    }
                    order_audit_.erase(audit);
                }
            }
            if (report.exec_type == ExecType::Trade && report.owner == "MarketMaker")
            {
                if (first_fill_order_id_ == 0) first_fill_order_id_ = report.order_id;
                // A fill means the queue in front of this order reached zero, which is the fact the whole
                // distribution is built on: the order was first in line and someone traded with it.
                const auto fill_audit = order_audit_.find(report.order_id);
                if (fill_audit != order_audit_.end() && fill_audit->second.profiling)
                {
                    fill_audit->second.min_queue_ahead = 0;
                    fill_audit->second.queue_at_end = 0;
                }
                ++trade_reports_;
                trade_report_quantity_ += report.executed_quantity();
            }
            ledger_.write_trade(report);
        });
}

void Pipeline::sample_resting_queues()
{
    // The map holds open orders only, so this costs a handful of lookups per event. It is the difference
    // between knowing one order's wait and knowing the distribution of waits, and it runs whether or not a
    // trace was asked for, because the distribution is the claim and the trace is the illustration.
    for (auto& entry : order_audit_)
    {
        OrderAudit& audit = entry.second;
        if (!audit.profiling) continue;
        const uint64_t queue = engine_.queue_ahead_at(audit.symbol, audit.side, audit.price);
        if (queue < audit.min_queue_ahead) audit.min_queue_ahead = queue;
    }
}

void Pipeline::handle_rejected_order(const exchange::Order& exchange_order,
                                     const StrategyOrder& strategy_order, uint64_t ts)
{
    // The engine refused the order, so nothing rests and no execution report will follow for it. The
    // reserved quantity under the position gate has to go back, the audit entries have to go, and the
    // strategy has to be told, because it added the order to its own open set when it was submitted. The
    // report is the same one a venue sends when it rejects an order, so the strategy's existing handling
    // does the cleanup, and the account is untouched because a refused order never trades.
    if (strategy_order.side == Side::Buy)
        pending_buy_quantity_ -=
            std::min(pending_buy_quantity_, static_cast<int64_t>(strategy_order.quantity));
    else if (strategy_order.side == Side::Sell)
        pending_sell_quantity_ -=
            std::min(pending_sell_quantity_, static_cast<int64_t>(strategy_order.quantity));
    order_audit_.erase(strategy_order.order_id);
    order_start_cycles_.erase(strategy_order.order_id);
    strategy_.on_order_update(ExecutionReport{.order_id = strategy_order.order_id,
                                              .side = exchange_order.side,
                                              .exec_type = ExecType::Cancelled,
                                              .symbol = strategy_order.symbol,
                                              .price = strategy_order.price,
                                              .quantity = strategy_order.quantity,
                                              .ts = ts,
                                              .owner = "MarketMaker"});
}

void Pipeline::process_event(const MarketEvent& event)
{
    apply_due_funding(std::visit([](const auto& value) { return value.ts; }, event));
    const auto exchange = std::visit([](const auto& value) { return value.exchange; }, event);
    if (!exchange.empty())
    {
        if (market_exchange_.empty())
            market_exchange_ = exchange;
        else if (market_exchange_ != exchange)
            throw std::invalid_argument("market event exchange does not match pipeline source");
    }

    std::visit(
        [this](const auto& value)
        {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, MarketTrade>)
            {
                const auto quote_it = latest_quotes_.find(value.symbol);
                if (quote_it == latest_quotes_.end())
                {
                    ++filtered_market_trades_;
                    return;
                }
                const BboQuote& quote = quote_it->second;
                const double mid = (quote.bid_price + quote.ask_price) / 2.0;
                const double deviation_bps = std::abs(value.price - mid) / mid * 10000.0;
                if (deviation_bps > 5.0)
                {
                    ++filtered_market_trades_;
                    return;
                }
                strategy_.on_market_trade(value, &quote);
                engine_.process_market_trade(value);
            }
            else if constexpr (std::is_same_v<Event, BboQuote>)
            {
                latest_quotes_[value.symbol] = value;
                order_book_.on_bbo(value);
                engine_.process_bbo(value);
                const auto top = order_book_.market_top(value.symbol);
                last_mid_ = (value.bid_price + value.ask_price) / 2.0;
                ++quote_cycle_;
                drain_mature_markouts();
                const double abs_inventory = static_cast<double>(std::abs(position_));
                ++inventory_samples_;
                execution_quality_.average_abs_inventory +=
                    (abs_inventory - execution_quality_.average_abs_inventory) /
                    static_cast<double>(inventory_samples_);
                process_top_of_book(value.symbol, value.ts, top);
            }
            else
            {
                return;
            }
        },
        event);
}

void Pipeline::process_top_of_book(const std::string& symbol, uint64_t ts, const TopOfBook& top)
{
    apply_due_funding(ts);
    current_ts_ = ts;
    submit_strategy_actions(symbol, ts, top);
    // The L1 path has no depth view to sample on, so it samples on its own updates; without this the
    // profile would report that no L1 quote ever moved toward the front, which would be an absence of
    // measurement rather than a fact.
    sample_resting_queues();
}

void Pipeline::process_l2_market_view(const L2MarketView& view)
{
    apply_due_funding(view.ts);
    current_ts_ = view.ts;
    if (view.top.bid_price > 0.0 && view.top.ask_price > 0.0)
    {
        const BboQuote quote{.ts = view.ts,
                             .symbol = view.symbol,
                             .bid_price = view.top.bid_price,
                             .bid_quantity = view.top.bid_size,
                             .ask_price = view.top.ask_price,
                             .ask_quantity = view.top.ask_size,
                             .exchange = {}};

        profiler_begin(Stage::TopBookKeeping);
        latest_quotes_.insert_or_assign(view.symbol, quote);
        profiler_end(Stage::TopBookKeeping);

        profiler_begin(Stage::Engine);
        engine_.process_l2_top(quote);
        profiler_end(Stage::Engine);

        // The report callback runs inside process_l2_top and reads last_mid_, so this
        // update has to stay after that call.
        profiler_begin(Stage::TopBookKeeping);
        last_mid_ = (view.top.bid_price + view.top.ask_price) / 2.0;
        ++quote_cycle_;
        // The report callback above pushes a markout for every fill, but only the L1 path drained
        // them, so on this path adverse selection stayed at zero however many fills there were. Same
        // five cycle horizon as the L1 branch.
        drain_mature_markouts();
        // The market's own prints, resolved by the same five cycle horizon. A print at or through a price is
        // what a resting quote there would have traded with, so its markout is the adverse selection a quote at
        // that distance would have suffered, and unlike our own fills it exists from the first event rather than
        // from the first one we win.
        if (measure_market_markouts_)
        {
            while (!pending_prints_.empty() && quote_cycle_ >= pending_prints_.front().start_cycle + 5)
            {
                const PassivePrint print = pending_prints_.front();
                pending_prints_.pop_front();
                const double print_markout =
                    print.side == Side::Buy ? last_mid_ - print.price : print.price - last_mid_;
                const std::size_t print_bucket =
                    print.bucket < 0 ? 0 : static_cast<std::size_t>(print.bucket);
                print_markout_ticks_quantity_[print_bucket] +=
                    print_markout / std::max(tick_size_, PRICE_TICK_SIZE) *
                    static_cast<double>(print.quantity);
                print_markout_quantity_[print_bucket] += static_cast<double>(print.quantity);
                ++print_samples_;
            }
            publish_markout_estimate();
        }
        profiler_end(Stage::TopBookKeeping);
    }

    profiler_begin(Stage::QueueAccounting);
    const uint64_t buy_queue = engine_.queue_ahead_consumed(Side::Buy);
    const uint64_t sell_queue = engine_.queue_ahead_consumed(Side::Sell);
    if (buy_queue > last_buy_queue_ahead_consumed_)
    {
        strategy_.on_queue_activity(Side::Buy, buy_queue - last_buy_queue_ahead_consumed_);
        last_buy_queue_ahead_consumed_ = buy_queue;
    }
    if (sell_queue > last_sell_queue_ahead_consumed_)
    {
        strategy_.on_queue_activity(Side::Sell, sell_queue - last_sell_queue_ahead_consumed_);
        last_sell_queue_ahead_consumed_ = sell_queue;
    }
    profiler_end(Stage::QueueAccounting);

    profiler_begin(Stage::Strategy);
    submit_strategy_actions(view.symbol, view.ts, strategy_.on_l2_market_view(view));
    profiler_end(Stage::Strategy);

    // Sampled last, so the row describes the queue the way this event left it: the engine has already
    // applied whatever quantity changes the view implied.
    ledger_.trace_sample("view", view.ts, engine_);
    sample_resting_queues();
}

void Pipeline::process_l2_market_trade(const MarketTrade& trade)
{
    apply_due_funding(trade.ts);
    current_ts_ = trade.ts;
    if (measure_market_markouts_ && trade.aggressor_side != Side::Unknown && last_mid_ > 0.0)
    {
        // The passive side of an aggressive sell is a resting bid, and a quote of ours there is exactly what the
        // print would have traded with. The distance is measured the way the fill buckets measure it, so the two
        // can be read against each other.
        const bool at_least_a_tick_away =
            std::abs(trade.price - last_mid_) >= std::max(tick_size_, PRICE_TICK_SIZE);
        const Side passive_side = trade.aggressor_side == Side::Sell ? Side::Buy : Side::Sell;
        pending_prints_.push_back(
            PassivePrint{.side = passive_side,
                         .price = trade.price,
                         .quantity = trade.quantity,
                         .start_cycle = quote_cycle_,
                         .bucket = (passive_side == Side::Buy ? 0 : 2) +
                                   (at_least_a_tick_away ? 0 : 1)});
    }
    strategy_.on_market_trade(trade);
    engine_.process_market_trade(trade);
    ledger_.trace_sample("trade", trade.ts, engine_);
    sample_resting_queues();
}

void Pipeline::drain_mature_markouts()
{
    // The observations are queued in cycle order, so the front is the oldest and the loop stops at the
    // first one whose horizon has not closed yet. Same five cycles on every path, and the arithmetic is
    // kept in the order it had when this was written twice, because the stdout hash is exact.
    while (!pending_markouts_.empty() &&
           quote_cycle_ >= pending_markouts_.front().start_cycle + 5)
    {
        const auto observation = pending_markouts_.front();
        pending_markouts_.pop_front();
        const double markout =
            observation.side == Side::Buy ? observation.price - last_mid_ : last_mid_ - observation.price;
        execution_quality_.adverse_selection += markout;
        execution_quality_.markout_quantity += static_cast<double>(observation.quantity);
        execution_quality_.markout_ticks_quantity +=
            markout / std::max(tick_size_, PRICE_TICK_SIZE) * static_cast<double>(observation.quantity);
        const double unit_value_at_mark = unit_notional_at(unit_notional_usd_, multiplier_, last_mid_);
        execution_quality_.markout_usd +=
            static_cast<double>(observation.quantity) / static_cast<double>(quantity_scale_) *
            unit_value_at_mark * markout / std::max(last_mid_, 1e-9);
        ++execution_quality_.markout_count;
        const std::size_t bucket =
            observation.bucket >= 0 ? static_cast<std::size_t>(observation.bucket) : 0;
        execution_quality_.markout_ticks_quantity_by_bucket[bucket] +=
            markout / std::max(tick_size_, PRICE_TICK_SIZE) * static_cast<double>(observation.quantity);
        execution_quality_.markout_quantity_by_bucket[bucket] += static_cast<double>(observation.quantity);
        ++execution_quality_.markout_count_by_bucket[bucket];
    }
}

void Pipeline::publish_markout_estimate()
{
    // Our own fills are what the gate has to be built on. The venue's prints were tried first as a proxy, and
    // on the mainline window the two disagree in sign in the far bucket, which is recorded in
    // docs/PERFORMANCE_HISTORY.md: a print at a distance is not the same event as a quote at that distance being
    // hit, so the measured cost of the second has to come from the second.
    markout_estimate_.buy_far_ticks = markout_bucket_ticks(execution_quality_, 0);
    markout_estimate_.buy_near_ticks = markout_bucket_ticks(execution_quality_, 1);
    markout_estimate_.sell_far_ticks = markout_bucket_ticks(execution_quality_, 2);
    markout_estimate_.sell_near_ticks = markout_bucket_ticks(execution_quality_, 3);
    markout_estimate_.samples = execution_quality_.markout_count;
    for (std::size_t bucket = 0; bucket < print_markout_average_ticks_.size(); ++bucket)
        print_markout_average_ticks_[bucket] = print_markout_quantity_[bucket] > 0.0
                                                   ? print_markout_ticks_quantity_[bucket] /
                                                         print_markout_quantity_[bucket]
                                                   : 0.0;
    strategy_.on_markout_estimate(markout_estimate_);
}

void Pipeline::apply_due_funding(uint64_t ts)
{
    if (funding_rates_ == nullptr) return;
    FundingRatePoint point;
    while (funding_rates_->next_settlement(ts, point))
    {
        ++funding_settlements_;
        // A settlement before the first price is applied to an account that holds nothing yet, so there is
        // no notional to charge and nothing to do; the settlement is still consumed, because the replay has
        // passed it and a later event must not apply it again.
        if (last_mid_ <= 0.0) continue;
        // The published rate is what longs pay; the accounting takes the rate a long earns on the notional,
        // so the sign is flipped once here and each position's own side then decides who pays whom.
        funding_flow_usd_ +=
            portfolio_.accrue_interval_flow(funding_rates_->symbol(), -point.rate, last_mid_);
    }
}

PnlSummary Pipeline::pnl_summary() const
{
    const auto metrics = portfolio_.metrics();
    PnlSummary pnl;
    pnl.closed_pnl_usd = metrics.realized_pnl;
    pnl.net_pnl_usd = metrics.realized_pnl + metrics.unrealized_pnl;
    pnl.gross_closed_pnl_usd = pnl.closed_pnl_usd + metrics.fees_paid;
    pnl.gross_pnl_usd = pnl.net_pnl_usd + metrics.fees_paid;
    pnl.maker_notional_usd = maker_notional_usd_;
    if (trade_reports_ > 0)
    {
        const double fills = static_cast<double>(trade_reports_);
        pnl.net_per_fill_usd = pnl.net_pnl_usd / fills;
        pnl.gross_closed_per_fill_usd = pnl.gross_closed_pnl_usd / fills;
    }
    // The maker rate that would have made the run exactly flat, on the notional its maker fills traded. A
    // negative answer is the statement the fee question is really asking about: no fee at all would still
    // lose money, and a rebate of that size would be needed to break even.
    if (maker_notional_usd_ > 0.0) pnl.breakeven_maker_rate = pnl.gross_pnl_usd / maker_notional_usd_;
    return pnl;
}

RunSummary Pipeline::summary() const
{
    // The account's result. `realized_pnl` is what completed, which is the part a market maker chose to
    // close, and `unrealized_pnl` is the position it was still holding when the input ended: the first is
    // the quoting result and the second is the bet the run left open, and the two are worth different
    // conclusions, so both are reported and neither is called "the PnL".
    PnlSummary pnl = pnl_summary();
    // The longest waiting order that is still in the book. The audit map holds the open orders, and their
    // lifetime is the one measure of patience this project has, so the maximum over it names the quote
    // whose queue is worth watching: it has survived every requote the run made.
    uint64_t longest_working_id = 0;
    uint64_t longest_working_cycles = 0;
    for (const auto& [order_id, audit] : order_audit_)
    {
        const uint64_t lifetime =
            quote_cycle_ >= audit.start_cycle ? quote_cycle_ - audit.start_cycle : 0;
        if (lifetime > longest_working_cycles)
        {
            longest_working_cycles = lifetime;
            longest_working_id = order_id;
        }
    }
    return {.submitted_orders = submitted_orders_,
            .submitted_quantity = submitted_quantity_,
            .cancel_requests = cancel_requests_,
            .trade_reports = trade_reports_,
            .trade_report_quantity = trade_report_quantity_,
            .queue_ahead_consumed = engine_.queue_ahead_consumed(),
            .buy_queue_ahead_levels_cleared = engine_.queue_ahead_levels_cleared(Side::Buy),
            .sell_queue_ahead_levels_cleared = engine_.queue_ahead_levels_cleared(Side::Sell),
            .buy_queue_from_quantity_changes =
                engine_.queue_ahead_from_quantity_changes(Side::Buy),
            .sell_queue_from_quantity_changes =
                engine_.queue_ahead_from_quantity_changes(Side::Sell),
            .filtered_market_trades = filtered_market_trades_,
            .fill_rate = metrics::compute_fill_rate(submitted_quantity_, trade_report_quantity_),
            .cancel_rate = metrics::compute_cancel_rate(submitted_orders_, cancel_requests_),
            .position_limit = position_limit_,
            .strategy_net_position = strategy_.net_position(),
            .strategy_position_mismatches = strategy_position_mismatches_,
            .risk_rejected_orders = risk_rejected_orders_,
            .max_exposure_limit_usd = hard_exposure_limit_usd_,
            .hard_limit_rejections = hard_limit_rejections_,
            .kill_switch_loss_usd = kill_switch_loss_usd_,
            .kill_switch_triggered = kill_switch_triggered_,
            .kill_switch_ts = kill_switch_ts_,
            .kill_switch_rejected_orders = kill_switch_rejected_orders_,
            .starting_cash_usd = starting_cash_usd_,
            .max_abs_exposure_usd = max_abs_exposure_usd_,
            .exposure_over_collateral_fills = exposure_over_collateral_fills_,
            .working_orders = strategy_.working_order_count(),
            .portfolio = portfolio_.metrics(),
            .pnl = pnl,
            .strategy = strategy_.metrics(),
            .execution_quality = execution_quality_,
            .funding_requested = funding_rates_ != nullptr,
            // A settlement the replay never reached leaves the consumed count short of the file, which is
            // visible here and is the honest reading: the series is longer than the window.
            .funding = {.points = funding_rates_ != nullptr ? funding_rates_->size() : 0,
                        .span_hours = funding_rates_ != nullptr ? funding_rates_->span_ms() / 3600000 : 0,
                        .sum_rate = funding_rates_ != nullptr ? funding_rates_->sum_rate() : 0.0,
                        .annualized_rate =
                            funding_rates_ != nullptr ? funding_rates_->annualized_rate() : 0.0,
                        .settlements = funding_settlements_,
                        .flow_usd = funding_flow_usd_},
            .gate_diagnostics_requested = gate_diagnostics_,
            .gate = {.fill_buy_near_ticks = markout_estimate_.buy_near_ticks,
                     .fill_buy_far_ticks = markout_estimate_.buy_far_ticks,
                     .fill_sell_near_ticks = markout_estimate_.sell_near_ticks,
                     .fill_sell_far_ticks = markout_estimate_.sell_far_ticks,
                     .fill_samples = markout_estimate_.samples,
                     .market_buy_near_ticks = print_markout_average_ticks_[1],
                     .market_buy_far_ticks = print_markout_average_ticks_[0],
                     .market_sell_near_ticks = print_markout_average_ticks_[3],
                     .market_sell_far_ticks = print_markout_average_ticks_[2],
                     .market_samples = print_samples_,
                     .markout_gate_skips = strategy_.markout_gate_skips(),
                     .flatten_orders = strategy_.flatten_orders(),
                     .flatten_quantity = strategy_.flatten_quantity()},
            .trace_requested = ledger_.trace_requested(),
            .first_fill_order_id = first_fill_order_id_,
            .longest_wait_filled_order_id = longest_wait_filled_order_id_,
            .longest_wait_filled_cycles = longest_wait_filled_cycles_,
            .longest_wait_working_order_id = longest_working_id,
            .longest_wait_working_cycles = longest_working_cycles,
            .queue_profile_orders = queue_profile_orders_,
            .queue_zero_orders = queue_zero_orders_,
            .queue_min_fraction_p50 = percentile(queue_min_fractions_, 0.50),
            .queue_min_fraction_p10 = percentile(queue_min_fractions_, 0.10),
            .queue_end_fraction_p50 = percentile(queue_end_fractions_, 0.50),
            .queue_trace = ledger_.trace_rows()};
}

void Pipeline::submit_strategy_actions(const std::string& symbol, uint64_t ts, const TopOfBook& top)
{
    submit_strategy_actions(symbol, ts, strategy_.on_top_of_book(symbol, top));
}

void Pipeline::submit_strategy_actions(const std::string& symbol, uint64_t ts,
                                       std::vector<StrategyOrder> orders)
{
    (void)symbol;

    for (uint64_t order_id : strategy_.cancel_requests())
    {
        if (!pending_cancel_orders_.insert(order_id).second) continue;
        ++cancel_requests_;
        const auto cancelled_audit = order_audit_.find(order_id);
        if (cancelled_audit != order_audit_.end() && cancelled_audit->second.profiling)
        {
            // Read before the cancel and not from the Cancelled report: the engine erases a price level once
            // no order rests in it, so a reading taken after the order is gone would report an empty queue
            // for a level that was not empty. This is the number that says how much queue was still in front
            // of a quote when the strategy gave up on it.
            cancelled_audit->second.queue_at_end = engine_.queue_ahead_at(
                cancelled_audit->second.symbol, cancelled_audit->second.side,
                cancelled_audit->second.price);
        }
        engine_.cancel_order(order_id);
    }

    // The kill switch, checked once a cycle before anything is submitted, and deliberately here rather than in
    // the strategy: a limit a strategy can reason about is a limit the strategy can talk itself out of. It
    // stops quoting and it does not flatten -- flattening has been measured on this engine and costs more than
    // it saves -- and the strategy's own cancels still run above, so what is already resting comes back on the
    // next cycle instead of staying at the venue.
    if (kill_switch_loss_usd_ > 0.0 && !kill_switch_triggered_)
    {
        const auto account = portfolio_.metrics();
        if (account.realized_pnl + account.unrealized_pnl <= -kill_switch_loss_usd_)
        {
            kill_switch_triggered_ = true;
            kill_switch_ts_ = ts;
        }
    }

    for (auto& order : orders)
    {
        if (kill_switch_triggered_)
        {
            ++kill_switch_rejected_orders_;
            continue;
        }
        // The hard exposure cap, in dollars rather than in shares because the same share count is three times
        // the exposure on a thousand dollar share. It marks the projected position at the order's own price,
        // which is the conservative side of the mark for an order that adds to the position, and it runs
        // before the position gate so that neither gate folds the other's pending quantity into its own check.
        if (hard_exposure_limit_usd_ > 0.0 && order.quantity > 0)
        {
            const int64_t signed_quantity =
                order.side == Side::Buy ? static_cast<int64_t>(order.quantity)
                                        : -static_cast<int64_t>(order.quantity);
            const int64_t projected = position_ + pending_buy_quantity_ - pending_sell_quantity_ +
                                      signed_quantity;
            const double marked =
                std::abs(static_cast<double>(projected)) / static_cast<double>(quantity_scale_) *
                unit_notional_at(unit_notional_usd_, multiplier_, order.price);
            if (marked > hard_exposure_limit_usd_)
            {
                ++hard_limit_rejections_;
                continue;
            }
        }
        // Pre-trade position gate. It counts the quantity already resting on the side that the order
        // adds to, because those orders can still fill, and a market maker that ignores that can post
        // risk it has no collateral for.
        if (position_limit_ > 0 && order.quantity > 0)
        {
            const int64_t signed_quantity =
                order.side == Side::Buy ? static_cast<int64_t>(order.quantity)
                                        : -static_cast<int64_t>(order.quantity);
            const int64_t projected = position_ + pending_buy_quantity_ - pending_sell_quantity_ +
                                      signed_quantity;
            if (projected > position_limit_ || projected < -position_limit_)
            {
                ++risk_rejected_orders_;
                continue;
            }
            if (order.side == Side::Buy)
                pending_buy_quantity_ += static_cast<int64_t>(order.quantity);
            else if (order.side == Side::Sell)
                pending_sell_quantity_ += static_cast<int64_t>(order.quantity);
        }
        order.order_id = next_order_id_++;
        auto exchange_order = OrderLedger::to_exchange_order(order, ts, "MarketMaker");
        ++submitted_orders_;
        submitted_quantity_ += order.quantity;
        const auto quote_it = latest_quotes_.find(order.symbol);
        if (quote_it != latest_quotes_.end())
        {
            const BboQuote& quote = quote_it->second;
            const double reference = order.side == Side::Buy ? quote.bid_price : quote.ask_price;
            const double distance = order.side == Side::Buy ? reference - order.price
                                                             : order.price - reference;
            ++execution_quality_.quote_observations;
            execution_quality_.total_quote_distance += std::max(0.0, distance);
            execution_quality_.total_quote_distance_ticks +=
                std::max(0.0, distance) / std::max(tick_size_, PRICE_TICK_SIZE);
            if (distance <= 0.5 * std::max(tick_size_, PRICE_TICK_SIZE))
                ++execution_quality_.bbo_quote_observations;
            // At the touch means the quote joined the venue's own best price, where the displayed queue
            // is deepest and the flow actually arrives; behind it means the spread protects the quote
            // while the queue in front clears. The two accounts are what a fill-rate question needs:
            // a low fill count means something different for each of them.
            if (distance <= 0.0)
                ++execution_quality_.quote_at_touch_orders;
            else
                ++execution_quality_.quote_behind_touch_orders;
        }
        ledger_.write_order(ts, order);
        strategy_.on_order_submitted(order);
        order_start_cycles_.insert_or_assign(order.order_id, quote_cycle_);
        order_audit_.insert_or_assign(order.order_id, OrderAudit{.start_cycle = quote_cycle_});
        ++execution_quality_.audited_orders;
        ledger_.trace_submit(order, ts, engine_);
        const uint64_t rejected_before = engine_.rejected_orders();
        engine_.send_order(exchange_order);
        if (engine_.rejected_orders() != rejected_before)
        {
            handle_rejected_order(exchange_order, order, ts);
        }
    }
}
