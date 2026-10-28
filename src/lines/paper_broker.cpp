#include "lines/paper_broker.h"

#include <utility>

std::string BrokerSummary::to_log_string() const
{
    return "orders_submitted=" + std::to_string(orders_submitted) +
           " orders_rejected=" + std::to_string(orders_rejected) +
           " fills_seen=" + std::to_string(fills_seen) +
           " fills_split=" + std::to_string(fills_split) +
           " fills_dropped=" + std::to_string(fills_dropped) +
           " outages=" + std::to_string(outages) + " orders_held=" + std::to_string(orders_held) +
           " orders_released=" + std::to_string(orders_released) +
           " cancels_while_held=" + std::to_string(cancels_while_held);
}

PaperBroker::PaperBroker(IMatchingEngine& venue, BrokerFaults faults)
    : venue_(venue), faults_(faults)
{
    // The venue's callback is taken over here rather than installed by the caller, because every report
    // has to pass through the broker whether or not a strategy asked for one.
    venue_.set_report_callback([this](const ExecutionReport& report) { on_venue_report(report); });
}

void PaperBroker::process_bbo(const BboQuote& quote)
{
    venue_.process_bbo(quote);
}

void PaperBroker::process_l2_top(const BboQuote& quote)
{
    venue_.process_l2_top(quote);
}

void PaperBroker::process_market_trade(const MarketTrade& trade)
{
    venue_.process_market_trade(trade);
}

void PaperBroker::set_report_callback(ReportCallback callback)
{
    consumer_ = std::move(callback);
}

uint64_t PaperBroker::rejected_orders() const
{
    return venue_.rejected_orders() + broker_rejects_;
}

uint64_t PaperBroker::queue_ahead_consumed() const
{
    return venue_.queue_ahead_consumed();
}

uint64_t PaperBroker::queue_ahead_consumed(Side side) const
{
    return venue_.queue_ahead_consumed(side);
}

uint64_t PaperBroker::queue_ahead_levels_cleared(Side side) const
{
    return venue_.queue_ahead_levels_cleared(side);
}

uint64_t PaperBroker::queue_ahead_from_quantity_changes(Side side) const
{
    return venue_.queue_ahead_from_quantity_changes(side);
}

uint64_t PaperBroker::queue_ahead_at(const std::string& symbol, Side side, double price) const
{
    return venue_.queue_ahead_at(symbol, side, price);
}

void PaperBroker::send_order(const exchange::Order& order)
{
    ++summary_.orders_submitted;
    // A refusal emits no report, exactly as the venue does, which is why the pipeline watches the reject
    // count change across a submission instead of waiting for a message that will not arrive.
    if (faults_.reject_every > 0 && summary_.orders_submitted % faults_.reject_every == 0)
    {
        ++summary_.orders_rejected;
        ++broker_rejects_;
        return;
    }

    if (!outage_seen_ && faults_.outage_after > 0 && accepted_orders_ >= faults_.outage_after)
    {
        outage_active_ = true;
        outage_seen_ = true;
        ++summary_.outages;
    }
    if (outage_active_)
    {
        // A hold of zero is a venue that never comes back, so everything from here is held.
        if (faults_.outage_holds == 0 || held_since_outage_ < faults_.outage_holds)
        {
            held_.push_back(order);
            ++held_since_outage_;
            ++summary_.orders_held;
            return;
        }
        // Reachable again: everything sent while it was not goes out first, in the order it was sent,
        // and then this order.
        outage_active_ = false;
        release_held();
    }

    ++accepted_orders_;
    venue_.send_order(order);
}

void PaperBroker::cancel_order(uint64_t order_id)
{
    // A cancel for an order the venue has not been given resolves against the hold, and it is answered:
    // the order is gone for good, so the strategy is owed the same report it would have received had the
    // venue cancelled it. Returning silently left the strategy waiting for a confirmation that could never
    // come, and it stopped quoting altogether.
    for (std::size_t index = 0; index < held_.size(); ++index)
    {
        if (held_[index].id != order_id) continue;
        const exchange::Order held = held_[index];
        held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(index));
        ++summary_.cancels_while_held;
        forward(ExecutionReport{.order_id = held.id,
                                .side = held.side,
                                .exec_type = ExecType::Cancelled,
                                .symbol = held.symbol,
                                .price = held.price,
                                .quantity = held.remaining,
                                .ts = held.ts,
                                .owner = held.owner,
                                .cancel_reason = CancelReason::User});
        return;
    }
    venue_.cancel_order(order_id);
}

OrderQuery PaperBroker::query_order(uint64_t order_id) const
{
    for (const exchange::Order& order : held_)
        if (order.id == order_id) return OrderQuery::Working;
    return venue_.query_order(order_id);
}

void PaperBroker::release_held()
{
    // Swapped out before sending, so a venue that reports a fill synchronously cannot find the queue it
    // is being drained from half emptied.
    std::deque<exchange::Order> releasing;
    releasing.swap(held_);
    summary_.orders_released += releasing.size();
    held_since_outage_ = 0;
    for (const exchange::Order& order : releasing) venue_.send_order(order);
}

void PaperBroker::record_execution(const ExecutionReport& report, uint64_t quantity, double fee)
{
    // Only this strategy's executions are the broker's statement of this strategy's session; anything
    // else the venue reports belongs to somebody else.
    if (report.owner != "MarketMaker") return;
    auto& symbol = statement_.symbols[report.symbol];
    symbol.net_quantity += report.side == exchange::Side::Buy ? static_cast<int64_t>(quantity)
                                                              : -static_cast<int64_t>(quantity);
    symbol.filled_quantity += quantity;
    ++symbol.fill_count;
    symbol.fees += fee;
    ++statement_.executions;
    statement_.fees += fee;
}

void PaperBroker::on_venue_report(const ExecutionReport& report)
{
    // Only this strategy's own fills are the broker's business: the venue reports the other side of every
    // print too, and a broker does not refuse, split or lose somebody else's execution. Those reports are
    // forwarded untouched, which is also what the pipeline expects of the venue.
    if (report.exec_type != ExecType::Trade || report.owner != "MarketMaker")
    {
        forward(report);
        return;
    }

    ++summary_.fills_seen;
    // The statement is written before anything decides whether the strategy hears about the fill,
    // because that difference is the whole point of a lost report.
    record_execution(report, report.quantity, report.fee);
    if (faults_.drop_every > 0 && summary_.fills_seen % faults_.drop_every == 0)
    {
        ++summary_.fills_dropped;
        return;
    }
    if (faults_.partial_every > 0 && summary_.fills_seen % faults_.partial_every == 0 &&
        report.quantity > 1)
    {
        const uint64_t first = report.quantity / 2;
        const double first_fee =
            report.fee * static_cast<double>(first) / static_cast<double>(report.quantity);
        ExecutionReport part = report;
        part.quantity = first;
        part.fee = first_fee;
        forward(part);
        part.quantity = report.quantity - first;
        // The remainder takes what is left of the fee rather than a second rounding, so the two prints
        // cost between them exactly what the one fill did.
        part.fee = report.fee - first_fee;
        forward(part);
        ++summary_.fills_split;
        return;
    }
    forward(report);
}

void PaperBroker::forward(const ExecutionReport& report)
{
    // Counted over the same population the portfolio books, so the two counts describe the same fills
    // and a difference between them is the pipeline losing or duplicating one.
    if (report.exec_type == ExecType::Trade && report.owner == "MarketMaker")
        ++statement_.forwarded_fills;
    if (consumer_) consumer_(report);
}
