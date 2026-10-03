#include "app/order_ledger.h"

#include "exchange/matching_engine.h"

namespace
{
std::string side_to_csv(Side side)
{
    return side == Side::Buy ? "B" : side == Side::Sell ? "S" : "N";
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
