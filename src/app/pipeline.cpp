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
            if (report.exec_type == ExecType::Resting) ledger_.mark_resting(report, engine_);
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
                accounting_.note_fill(report.side == exchange::Side::Buy ? Side::Buy : Side::Sell,
                                      report.price, report.executed_quantity(), quote_cycle_,
                                      markout_bucket);
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
            }
            const double owed =
                ledger_.on_report(report, quote_cycle_, execution_quality_, quantity_scale_,
                                  commission_per_unit_, min_commission_per_order_);
            if (owed > 0.0) portfolio_.charge_fee(owed);
            if (report.exec_type == ExecType::Trade && report.owner == "MarketMaker")
            {
                ++trade_reports_;
                trade_report_quantity_ += report.executed_quantity();
            }
            ledger_.write_trade(report);
        });
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
    ledger_.forget_order(strategy_order.order_id);
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
    accounting_.apply_due_funding(std::visit([](const auto& value) { return value.ts; }, event),
                                  portfolio_, last_mid_);
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
                accounting_.drain_mature_markouts(quote_cycle_, last_mid_, tick_size_, unit_notional_usd_, multiplier_, quantity_scale_, execution_quality_);
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
    accounting_.apply_due_funding(ts, portfolio_, last_mid_);
    current_ts_ = ts;
    submit_strategy_actions(symbol, ts, top);
    // The L1 path has no depth view to sample on, so it samples on its own updates; without this the
    // profile would report that no L1 quote ever moved toward the front, which would be an absence of
    // measurement rather than a fact.
    ledger_.sample_resting(engine_);
}

void Pipeline::process_l2_market_view(const L2MarketView& view)
{
    accounting_.apply_due_funding(view.ts, portfolio_, last_mid_);
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
        accounting_.drain_mature_markouts(quote_cycle_, last_mid_, tick_size_, unit_notional_usd_, multiplier_, quantity_scale_, execution_quality_);
        // The market's own prints, resolved by the same five cycle horizon. A print at or through a price is
        // what a resting quote there would have traded with, so its markout is the adverse selection a quote at
        // that distance would have suffered, and unlike our own fills it exists from the first event rather than
        // from the first one we win.
        if (accounting_.market_markouts_enabled())
        {
            accounting_.drain_mature_prints(quote_cycle_, last_mid_, tick_size_);
            accounting_.publish_markout_estimate(execution_quality_, strategy_);
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
    ledger_.sample_resting(engine_);
}

void Pipeline::process_l2_market_trade(const MarketTrade& trade)
{
    accounting_.apply_due_funding(trade.ts, portfolio_, last_mid_);
    current_ts_ = trade.ts;
    if (accounting_.market_markouts_enabled() && trade.aggressor_side != Side::Unknown &&
        last_mid_ > 0.0)
    {
        // The passive side of an aggressive sell is a resting bid, and a quote of ours there is exactly what the
        // print would have traded with. The distance is measured the way the fill buckets measure it, so the two
        // can be read against each other.
        const bool at_least_a_tick_away =
            std::abs(trade.price - last_mid_) >= std::max(tick_size_, PRICE_TICK_SIZE);
        const Side passive_side = trade.aggressor_side == Side::Sell ? Side::Buy : Side::Sell;
        accounting_.note_market_print(passive_side, trade.price, trade.quantity, quote_cycle_,
                                      (passive_side == Side::Buy ? 0 : 2) +
                                          (at_least_a_tick_away ? 0 : 1));
    }
    strategy_.on_market_trade(trade);
    engine_.process_market_trade(trade);
    ledger_.trace_sample("trade", trade.ts, engine_);
    ledger_.sample_resting(engine_);
}

RunSummary Pipeline::summary() const
{
    // The account's result. `realized_pnl` is what completed, which is the part a market maker chose to
    // close, and `unrealized_pnl` is the position it was still holding when the input ended: the first is
    // the quoting result and the second is the bet the run left open, and the two are worth different
    // conclusions, so both are reported and neither is called "the PnL".
    PnlSummary pnl = RunAccounting::pnl_summary(portfolio_, maker_notional_usd_, trade_reports_);
    // The longest waiting order that is still in the book: the audit map holds the open orders, so the
    // maximum over it names the quote whose queue is worth watching. It has survived every requote the run
    // made.
    const auto [longest_working_id, longest_working_cycles] =
        ledger_.longest_working_order(quote_cycle_);
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
            .funding_requested = accounting_.funding_requested(),
            // A settlement the replay never reached leaves the consumed count short of the file, which is
            // visible here and is the honest reading: the series is longer than the window.
            .funding = {.points = accounting_.funding_points(),
                        .span_hours = accounting_.funding_span_hours(),
                        .sum_rate = accounting_.funding_sum_rate(),
                        .annualized_rate = accounting_.funding_annualized_rate(),
                        .settlements = accounting_.funding_settlements(),
                        .flow_usd = accounting_.funding_flow_usd()},
            .gate_diagnostics_requested = gate_diagnostics_,
            .gate = {.fill_buy_near_ticks = accounting_.markout_estimate().buy_near_ticks,
                     .fill_buy_far_ticks = accounting_.markout_estimate().buy_far_ticks,
                     .fill_sell_near_ticks = accounting_.markout_estimate().sell_near_ticks,
                     .fill_sell_far_ticks = accounting_.markout_estimate().sell_far_ticks,
                     .fill_samples = accounting_.markout_estimate().samples,
                     .market_buy_near_ticks = accounting_.print_markout_average_ticks()[1],
                     .market_buy_far_ticks = accounting_.print_markout_average_ticks()[0],
                     .market_sell_near_ticks = accounting_.print_markout_average_ticks()[3],
                     .market_sell_far_ticks = accounting_.print_markout_average_ticks()[2],
                     .market_samples = accounting_.print_samples(),
                     .markout_gate_skips = strategy_.markout_gate_skips(),
                     .flatten_orders = strategy_.flatten_orders(),
                     .flatten_quantity = strategy_.flatten_quantity()},
            .trace_requested = ledger_.trace_requested(),
            .first_fill_order_id = ledger_.first_fill_order_id(),
            .longest_wait_filled_order_id = ledger_.longest_wait_filled_order_id(),
            .longest_wait_filled_cycles = ledger_.longest_wait_filled_cycles(),
            .longest_wait_working_order_id = longest_working_id,
            .longest_wait_working_cycles = longest_working_cycles,
            .queue_profile_orders = ledger_.queue_profile_orders(),
            .queue_zero_orders = ledger_.queue_zero_orders(),
            .queue_min_fraction_p50 = percentile(ledger_.queue_min_fractions(), 0.50),
            .queue_min_fraction_p10 = percentile(ledger_.queue_min_fractions(), 0.10),
            .queue_end_fraction_p50 = percentile(ledger_.queue_end_fractions(), 0.50),
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
        if (!ledger_.begin_cancel(order_id, engine_)) continue;
        ++cancel_requests_;
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
        ledger_.open_order(order.order_id, quote_cycle_, execution_quality_);
        ledger_.trace_submit(order, ts, engine_);
        const uint64_t rejected_before = engine_.rejected_orders();
        engine_.send_order(exchange_order);
        if (engine_.rejected_orders() != rejected_before)
        {
            handle_rejected_order(exchange_order, order, ts);
        }
    }
}
