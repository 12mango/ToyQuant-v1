#include "lines/order_gateway.h"

#include <utility>
#include <vector>

std::string GatewaySummary::to_log_string() const
{
    return "submissions=" + std::to_string(submissions) +
           " duplicates_suppressed=" + std::to_string(duplicates_suppressed) +
           " acks=" + std::to_string(acks) + " ack_timeouts=" + std::to_string(ack_timeouts) +
           " retries=" + std::to_string(retries) +
           " retry_rejections=" + std::to_string(retry_rejections) +
           " venue_rejects=" + std::to_string(venue_rejects) +
           " cancels=" + std::to_string(cancels) +
           " cancels_after_fill=" + std::to_string(cancels_after_fill);
}

OrderGateway::OrderGateway(IMatchingEngine& venue, GatewayConfig config)
    : venue_(venue), config_(config)
{
    // Taken over here rather than installed by the caller, because every answer has to pass through the
    // gateway whether or not a strategy asked for reports.
    venue_.set_report_callback([this](const ExecutionReport& report) { on_venue_report(report); });
}

void OrderGateway::set_report_callback(ReportCallback callback)
{
    consumer_ = std::move(callback);
}

void OrderGateway::process_bbo(const BboQuote& quote)
{
    venue_.process_bbo(quote);
    sweep_timeouts(quote.ts);
}

void OrderGateway::process_l2_top(const BboQuote& quote)
{
    venue_.process_l2_top(quote);
    sweep_timeouts(quote.ts);
}

void OrderGateway::process_market_trade(const MarketTrade& trade)
{
    venue_.process_market_trade(trade);
    sweep_timeouts(trade.ts);
}

void OrderGateway::send_order(const exchange::Order& order)
{
    ++summary_.submissions;
    // The client id is the strategy's order id, and it is entered here before the venue is called. A
    // second submission under an id still in the table is a retry: the venue already has the order, so
    // forwarding it again would be asking for a second position under the same name.
    if (in_flight_.contains(order.id))
    {
        ++summary_.duplicates_suppressed;
        return;
    }
    in_flight_.emplace(order.id, Tracked{.order = order,
                                          .submitted_ts = order.ts,
                                          .remaining = order.remaining});

    // A refusal emits no report, so the only way the table learns that the order never arrived is the
    // venue's reject counter moving across the call. The pipeline watches the same counter.
    const uint64_t rejected_before = venue_.rejected_orders();
    venue_.send_order(order);
    if (venue_.rejected_orders() != rejected_before)
    {
        ++summary_.venue_rejects;
        in_flight_.erase(order.id);
    }
}

void OrderGateway::cancel_order(uint64_t order_id)
{
    ++summary_.cancels;
    if (!in_flight_.contains(order_id))
    {
        // The gateway has already seen this order finish, so there is nothing at the venue to cancel.
        // Counting it is more useful than a call the venue would silently ignore.
        ++summary_.cancels_after_fill;
        return;
    }
    venue_.cancel_order(order_id);
}

void OrderGateway::on_venue_report(const ExecutionReport& report)
{
    const auto entry = in_flight_.find(report.order_id);
    if (entry != in_flight_.end())
    {
        Tracked& tracked = entry->second;
        if (!tracked.answered)
        {
            tracked.answered = true;
            ++summary_.acks;
        }
        switch (report.exec_type)
        {
            case ExecType::Trade:
                // A Trade carries what this print executed rather than what is left of the order.
                tracked.remaining = report.quantity >= tracked.remaining
                                        ? 0
                                        : tracked.remaining - report.quantity;
                break;
            // These carry the order's remaining quantity, so the report is the truth about the order.
            case ExecType::Resting:
            case ExecType::PartialFill:
                tracked.remaining = report.quantity;
                break;
            case ExecType::Filled:
            case ExecType::Cancelled:
                tracked.remaining = 0;
                break;
        }
        if (tracked.remaining == 0) in_flight_.erase(entry);
    }
    forward(report);
}

void OrderGateway::forward(const ExecutionReport& report)
{
    if (consumer_) consumer_(report);
}

void OrderGateway::sweep_timeouts(uint64_t now_ts)
{
    if (config_.ack_timeout_us == 0) return;
    // Collected before anything is sent: a retry can be answered synchronously, and that answer erases
    // from the same table this walk is reading.
    std::vector<exchange::Order> due;
    for (auto& [id, tracked] : in_flight_)
    {
        (void)id;
        if (tracked.answered || tracked.timed_out) continue;
        // A feed whose timestamps went backwards cannot make an order older than it is.
        if (now_ts < tracked.submitted_ts) continue;
        if (now_ts - tracked.submitted_ts < config_.ack_timeout_us) continue;
        tracked.timed_out = true;
        ++summary_.ack_timeouts;
        if (config_.retry_on_timeout && !tracked.retried)
        {
            tracked.retried = true;
            ++summary_.retries;
            due.push_back(tracked.order);
        }
    }
    for (const exchange::Order& order : due)
    {
        // The same client id, not a new one: the venue refuses the duplicate when it already has the
        // order and accepts it when the first copy was lost. Either way the position moves once.
        const uint64_t rejected_before = venue_.rejected_orders();
        venue_.send_order(order);
        if (venue_.rejected_orders() != rejected_before) ++summary_.retry_rejections;
    }
}
