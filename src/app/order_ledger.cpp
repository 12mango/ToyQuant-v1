#include "app/order_ledger.h"

#include <algorithm>
#include <cstddef>

#include "common/instrument_spec.h"
#include "exchange/matching_engine.h"

namespace
{
std::string side_to_csv(Side side)
{
    return side == Side::Buy ? "B" : side == Side::Sell ? "S" : "N";
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

OrderLedger::OrderLedger(std::ofstream& orders_out, std::ofstream& trades_out)
    : orders_out_(orders_out), trades_out_(trades_out)
{
}

exchange::Order OrderLedger::to_exchange_order(const StrategyOrder& strategy_order, uint64_t ts,
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

void OrderLedger::write_order(uint64_t ts, const StrategyOrder& order)
{
    orders_out_ << ts << "," << order.symbol << "," << side_to_csv(order.side) << "," << order.price
                << "," << order.quantity << "," << order.order_id << "\n";
}

void OrderLedger::write_trade(const ExecutionReport& report)
{
    if (report.exec_type != ExecType::Trade || report.owner != "MarketMaker") return;
    const char* role = report.liquidity_role == LiquidityRole::Maker   ? "maker"
                       : report.liquidity_role == LiquidityRole::Taker ? "taker"
                                                                       : "unknown";
    trades_out_ << report.ts << "," << report.symbol << ","
                << (report.side == exchange::Side::Buy ? "B" : "S") << "," << report.price << ","
                << report.quantity << "," << report.order_id << "," << role << "," << report.fee
                << "\n";
}

void OrderLedger::trace_submit(const StrategyOrder& order, uint64_t ts, IMatchingEngine& engine)
{
    if (order.order_id != trace_order_id_) return;
    trace_symbol_ = order.symbol;
    trace_side_ = order.side;
    trace_price_ = order.price;
    trace_level_known_ = true;
    trace_last_queue_ = engine.queue_ahead_at(trace_symbol_, trace_side_, trace_price_);
    trace_rows_.push_back(TraceRow{.ts = ts,
                                   .event = "submit",
                                   .side = trace_side_,
                                   .price = trace_price_,
                                   .change = static_cast<int64_t>(order.quantity),
                                   .queue_ahead = trace_last_queue_});
}

void OrderLedger::trace_sample(const char* event, uint64_t ts, IMatchingEngine& engine)
{
    if (trace_order_id_ == 0 || !trace_level_known_) return;
    const uint64_t queue = engine.queue_ahead_at(trace_symbol_, trace_side_, trace_price_);
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

void OrderLedger::trace_report(const ExecutionReport& report, uint64_t current_ts,
                               IMatchingEngine& engine)
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
        trace_level_known_ ? engine.queue_ahead_at(trace_symbol_, trace_side_, trace_price_) : 0;
    const uint64_t trace_ts = current_ts != 0 ? current_ts : report.ts;
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
void OrderLedger::open_order(uint64_t order_id, uint64_t start_cycle, ExecutionQualityMetrics& quality)
{
    order_start_cycles_.insert_or_assign(order_id, start_cycle);
    order_audit_.insert_or_assign(order_id, OrderAudit{.start_cycle = start_cycle});
    ++quality.audited_orders;
}

void OrderLedger::forget_order(uint64_t order_id)
{
    order_audit_.erase(order_id);
    order_start_cycles_.erase(order_id);
}

void OrderLedger::mark_resting(const ExecutionReport& report, IMatchingEngine& engine)
{
    const auto resting_audit = order_audit_.find(report.order_id);
    if (resting_audit == order_audit_.end()) return;
    OrderAudit& audit = resting_audit->second;
    audit.profiling = true;
    audit.symbol = report.symbol;
    audit.side = report.side == exchange::Side::Buy ? Side::Buy : Side::Sell;
    audit.price = report.price;
    // The queue this order joined behind. The engine has just created the level, and the level exists
    // only while one of our orders rests in it, which is why both ends of the measurement are taken
    // while the order is still there.
    audit.queue_at_rest = engine.queue_ahead_at(audit.symbol, audit.side, audit.price);
    audit.min_queue_ahead = audit.queue_at_rest;
}

bool OrderLedger::begin_cancel(uint64_t order_id, IMatchingEngine& engine)
{
    if (!pending_cancel_orders_.insert(order_id).second) return false;
    const auto cancelled_audit = order_audit_.find(order_id);
    if (cancelled_audit != order_audit_.end() && cancelled_audit->second.profiling)
    {
        // Read before the cancel and not from the Cancelled report: the engine erases a price level once
        // no order rests in it, so a reading taken after the order is gone would report an empty queue for
        // a level that was not empty. This is the number that says how much queue was still in front of a
        // quote when the strategy gave up on it.
        cancelled_audit->second.queue_at_end = engine.queue_ahead_at(
            cancelled_audit->second.symbol, cancelled_audit->second.side, cancelled_audit->second.price);
    }
    return true;
}

void OrderLedger::sample_resting(IMatchingEngine& engine)
{
    // The map holds open orders only, so this is a handful of lookups per event.
    for (auto& entry : order_audit_)
    {
        OrderAudit& audit = entry.second;
        if (!audit.profiling) continue;
        const uint64_t queue = engine.queue_ahead_at(audit.symbol, audit.side, audit.price);
        if (queue < audit.min_queue_ahead) audit.min_queue_ahead = queue;
    }
}

std::pair<uint64_t, uint64_t> OrderLedger::longest_working_order(uint64_t quote_cycle) const
{
    uint64_t longest_working_id = 0;
    uint64_t longest_working_cycles = 0;
    for (const auto& [order_id, audit] : order_audit_)
    {
        const uint64_t lifetime =
            quote_cycle >= audit.start_cycle ? quote_cycle - audit.start_cycle : 0;
        if (lifetime > longest_working_cycles)
        {
            longest_working_cycles = lifetime;
            longest_working_id = order_id;
        }
    }
    return {longest_working_id, longest_working_cycles};
}

double OrderLedger::on_report(const ExecutionReport& report, uint64_t quote_cycle,
                              ExecutionQualityMetrics& quality, double quantity_scale,
                              double commission_per_unit, double min_commission_per_order)
{
    if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
    {
        const auto start = order_start_cycles_.find(report.order_id);
        if (start != order_start_cycles_.end())
        {
            const uint64_t lifetime = quote_cycle >= start->second ? quote_cycle - start->second : 0;
            quality.total_quote_lifetime += lifetime;
            quality.max_quote_lifetime = std::max(quality.max_quote_lifetime, lifetime);
            order_start_cycles_.erase(start);
        }
        pending_cancel_orders_.erase(report.order_id);
    }

    double owed = 0.0;
    auto audit = order_audit_.find(report.order_id);
    if (audit != order_audit_.end())
    {
        if (report.exec_type == ExecType::Trade) audit->second.filled = true;
        if (report.exec_type == ExecType::Trade)
            audit->second.filled_quantity += report.executed_quantity();
        if (report.exec_type == ExecType::Trade && commission_per_unit > 0.0)
        {
            // The broker's own commission, accumulated per order so that the floor can be applied to the
            // order's total when it ends. The venue's rate is not part of it: a broker's floor is a floor
            // on its commission.
            const double units = quantity_scale > 0
                                     ? static_cast<double>(report.executed_quantity()) /
                                           static_cast<double>(quantity_scale)
                                     : static_cast<double>(report.executed_quantity());
            audit->second.commission += units * commission_per_unit;
        }
        if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
        {
            const uint64_t lifetime = quote_cycle >= audit->second.start_cycle
                                          ? quote_cycle - audit->second.start_cycle
                                          : 0;
            quality.total_order_lifetime_cycles += lifetime;
            if (audit->second.filled && lifetime > longest_wait_filled_cycles_)
            {
                longest_wait_filled_cycles_ = lifetime;
                longest_wait_filled_order_id_ = report.order_id;
            }
            if (report.exec_type == ExecType::Cancelled && audit->second.filled)
            {
                ++quality.cancelled_after_fill_orders;
            }
            else if (report.exec_type == ExecType::Cancelled)
            {
                ++quality.cancelled_before_fill_orders;
            }
            if (report.exec_type == ExecType::Filled)
                ++quality.audited_filled_orders;
            else
                ++quality.audited_cancelled_orders;
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
            // The order instance, not the fill, is what a broker's minimum per order is charged on, so it
            // is counted once here, when the order ends, and bucketed by how much it filled. A
            // modification or a cancel and replace makes a new instance, which is why this counter and the
            // fill count are not the same number.
            if (audit->second.filled)
            {
                ++quality.orders_that_filled;
                const std::size_t bucket = fill_size_bucket(audit->second.filled_quantity);
                ++quality.filled_order_count_by_size[bucket];
                quality.filled_order_quantity_by_size[bucket] += audit->second.filled_quantity;
                // The floor, which is why this counter exists: an order that filled less than the floor's
                // worth of shares owes the difference, and the order ending is the only moment the total
                // is known.
                const double shortfall =
                    min_commission_shortfall(min_commission_per_order, audit->second.commission);
                if (shortfall > 0.0)
                {
                    owed = shortfall;
                    ++quality.min_commission_orders;
                    quality.min_commission_paid += shortfall;
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
    }
    return owed;
}

