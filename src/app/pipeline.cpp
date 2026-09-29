#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

#include "backtest/performance.h"
#include "exchange/execution_report.h"

namespace
{
std::string side_to_csv(Side side)
{
    return side == Side::Buy ? "B" : side == Side::Sell ? "S" : "N";
}

exchange::Order to_exchange_order(const StrategyOrder& strategy_order, uint64_t ts,
                                  const std::string& owner)
{
    exchange::Order order{};
    order.id = strategy_order.order_id;
    order.symbol = strategy_order.symbol;
    order.side = strategy_order.side == Side::Buy ? exchange::Side::Buy : exchange::Side::Sell;
    order.type = exchange::OrderType::Limit;
    order.price = strategy_order.price;
    order.qty = strategy_order.quantity;
    order.remaining = strategy_order.quantity;
    order.ts = ts;
    order.owner = owner;
    return order;
}

void write_order_csv_row(std::ofstream& out, uint64_t ts, const StrategyOrder& order)
{
    out << ts << "," << order.symbol << "," << side_to_csv(order.side) << "," << order.price << ","
        << order.quantity << "," << order.order_id << "\n";
}

void write_trade_csv_row(std::ofstream& out, const ExecutionReport& report)
{
    if (report.exec_type != ExecType::Trade || report.owner != "MarketMaker") return;
    const char* role = report.liquidity_role == LiquidityRole::Maker   ? "maker"
                       : report.liquidity_role == LiquidityRole::Taker ? "taker"
                                                                       : "unknown";
    out << report.ts << "," << report.symbol << ","
        << (report.side == exchange::Side::Buy ? "B" : "S") << "," << report.price << ","
        << report.quantity << "," << report.order_id << "," << role << "," << report.fee << "\n";
}

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
    : orders_out_(orders_out),
      trades_out_(trades_out),
      order_book_(order_book),
      strategy_(strategy),
      engine_(engine),
      portfolio_(portfolio),
      tick_size_(tick_size)
{
    engine_.set_report_callback(
        [this](const ExecutionReport& report)
        {
            record_trace_report(report);
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
                    unit_notional_at(unit_notional_usd_, report.price);
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
                const double unit_value_at_fill = unit_notional_at(unit_notional_usd_, report.price);
                execution_quality_.captured_edge_usd +=
                    executed / static_cast<double>(quantity_scale_) * unit_value_at_fill *
                    unit_edge / std::max(report.price, 1e-9);
                pending_markouts_.push_back(
                    {report.side == exchange::Side::Buy ? Side::Buy : Side::Sell, report.price,
                     report.executed_quantity(), quote_cycle_});
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
            write_trade_csv_row(trades_out_, report);
        });
}

void Pipeline::record_trace_sample(const char* event, uint64_t ts)
{
    if (trace_order_id_ == 0 || !trace_level_known_) return;
    const uint64_t queue = engine_.queue_ahead_at(trace_symbol_, trace_side_, trace_price_);
    if (queue == trace_last_queue_) return;
    // A positive change means the queue shrank, which is the direction that leads to a fill.
    trace_rows_.push_back(TraceRow{.ts = ts,
                                   .event = event,
                                   .side = trace_side_,
                                   .price = trace_price_,
                                   .change = static_cast<int64_t>(trace_last_queue_) -
                                             static_cast<int64_t>(queue),
                                   .queue_ahead = queue});
    trace_last_queue_ = queue;
}

void Pipeline::record_trace_report(const ExecutionReport& report)
{
    if (trace_order_id_ == 0 || report.order_id != trace_order_id_) return;
    const char* event = report.exec_type == ExecType::Trade       ? "fill"
                        : report.exec_type == ExecType::Resting    ? "rest"
                        : report.exec_type == ExecType::Cancelled  ? "cancel"
                        : report.exec_type == ExecType::Filled     ? "fully_filled"
                                                                   : "report";
    const Side side = trace_level_known_
                          ? trace_side_
                          : (report.side == exchange::Side::Buy ? Side::Buy : Side::Sell);
    // For a rest this reads the queue the engine just made the order join behind, which is the number the
    // whole trace is about: a quote's fill depends on it, not only on its price.
    const uint64_t queue =
        trace_level_known_ ? engine_.queue_ahead_at(trace_symbol_, trace_side_, trace_price_) : 0;
    const uint64_t trace_ts = current_ts_ != 0 ? current_ts_ : report.ts;
    trace_rows_.push_back(TraceRow{.ts = trace_ts,
                                   .event = event,
                                   .side = side,
                                   .price = report.price,
                                   .change = static_cast<int64_t>(report.executed_quantity()),
                                   .queue_ahead = queue});
    trace_last_queue_ = queue;
    if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
        trace_level_known_ = false;
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
                while (!pending_markouts_.empty() &&
                       quote_cycle_ >= pending_markouts_.front().start_cycle + 5)
                {
                    const auto observation = pending_markouts_.front();
                    pending_markouts_.pop_front();
                    const double markout = observation.side == Side::Buy
                                               ? observation.price - last_mid_
                                               : last_mid_ - observation.price;
                    execution_quality_.adverse_selection += markout;
                    execution_quality_.markout_quantity +=
                        static_cast<double>(observation.quantity);
                    execution_quality_.markout_ticks_quantity +=
                        markout / std::max(tick_size_, PRICE_TICK_SIZE) *
                        static_cast<double>(observation.quantity);
                    const double unit_value_at_mark = unit_notional_at(unit_notional_usd_, last_mid_);
                    execution_quality_.markout_usd +=
                        static_cast<double>(observation.quantity) /
                        static_cast<double>(quantity_scale_) * unit_value_at_mark * markout /
                        std::max(last_mid_, 1e-9);
                    ++execution_quality_.markout_count;
                }
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
    current_ts_ = ts;
    submit_strategy_actions(symbol, ts, top);
    // The L1 path has no depth view to sample on, so it samples on its own updates; without this the
    // profile would report that no L1 quote ever moved toward the front, which would be an absence of
    // measurement rather than a fact.
    sample_resting_queues();
}

void Pipeline::process_l2_market_view(const L2MarketView& view)
{
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
        while (!pending_markouts_.empty() &&
               quote_cycle_ >= pending_markouts_.front().start_cycle + 5)
        {
            const auto observation = pending_markouts_.front();
            pending_markouts_.pop_front();
            const double markout = observation.side == Side::Buy ? observation.price - last_mid_
                                                                 : last_mid_ - observation.price;
            execution_quality_.adverse_selection += markout;
            execution_quality_.markout_quantity += static_cast<double>(observation.quantity);
            execution_quality_.markout_ticks_quantity +=
                markout / std::max(tick_size_, PRICE_TICK_SIZE) *
                static_cast<double>(observation.quantity);
            const double unit_value_at_mark = unit_notional_at(unit_notional_usd_, last_mid_);
            execution_quality_.markout_usd +=
                static_cast<double>(observation.quantity) /
                static_cast<double>(quantity_scale_) * unit_value_at_mark * markout /
                std::max(last_mid_, 1e-9);
            ++execution_quality_.markout_count;
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
    record_trace_sample("view", view.ts);
    sample_resting_queues();
}

void Pipeline::process_l2_market_trade(const MarketTrade& trade)
{
    current_ts_ = trade.ts;
    strategy_.on_market_trade(trade);
    engine_.process_market_trade(trade);
    record_trace_sample("trade", trade.ts);
    sample_resting_queues();
}

RunSummary Pipeline::summary() const
{
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
            .starting_cash_usd = starting_cash_usd_,
            .max_abs_exposure_usd = max_abs_exposure_usd_,
            .exposure_over_collateral_fills = exposure_over_collateral_fills_,
            .working_orders = strategy_.working_order_count(),
            .portfolio = portfolio_.metrics(),
            .strategy = strategy_.metrics(),
            .execution_quality = execution_quality_,
            .trace_requested = trace_order_id_ != 0,
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
            .queue_trace = trace_rows_};
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

    for (auto& order : orders)
    {
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
        auto exchange_order = to_exchange_order(order, ts, "MarketMaker");
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
        write_order_csv_row(orders_out_, ts, order);
        strategy_.on_order_submitted(order);
        order_start_cycles_.insert_or_assign(order.order_id, quote_cycle_);
        order_audit_.insert_or_assign(order.order_id, OrderAudit{.start_cycle = quote_cycle_});
        ++execution_quality_.audited_orders;
        if (order.order_id == trace_order_id_)
        {
            // The level is learned here so that the reports the engine is about to emit for this order can
            // already name the queue they are about. The submit row records the intent, with the queue the
            // venue was displaying at that price before our order joined it; the rest row that follows
            // records the queue the model actually made us join behind.
            trace_symbol_ = order.symbol;
            trace_side_ = order.side;
            trace_price_ = order.price;
            trace_level_known_ = true;
            trace_last_queue_ = engine_.queue_ahead_at(trace_symbol_, trace_side_, trace_price_);
            trace_rows_.push_back(TraceRow{.ts = ts,
                                           .event = "submit",
                                           .side = trace_side_,
                                           .price = trace_price_,
                                           .change = static_cast<int64_t>(order.quantity),
                                           .queue_ahead = trace_last_queue_});
        }
        const uint64_t rejected_before = engine_.rejected_orders();
        engine_.send_order(exchange_order);
        if (engine_.rejected_orders() != rejected_before)
        {
            handle_rejected_order(exchange_order, order, ts);
        }
    }
}
