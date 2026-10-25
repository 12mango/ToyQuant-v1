// Two properties of the gateway matter, and both are checked here: with every knob at zero it is
// invisible, and a retry of an order the venue has not answered cannot become a second position. The
// second one is the whole reason the layer exists, so it is checked against the venue's own reject
// counter rather than against the gateway's opinion of itself.
#include <cassert>
#include <cstdint>
#include <vector>

#include "exchange/matching_engine.h"
#include "lines/order_gateway.h"

namespace
{
constexpr const char* kSymbol = "EURUSD";
// Below the bid the test quotes, so the order rests instead of crossing the external book.
constexpr double kPrice = 1.09000;

exchange::Order resting_buy(uint64_t id, uint64_t ts)
{
    return {id,      kSymbol, exchange::Side::Buy, exchange::OrderType::Limit, kPrice,
            100,     100,     ts,                   "MarketMaker"};
}

BboQuote quote_at(uint64_t ts)
{
    return {ts, kSymbol, 1.10000, 10, 1.10010, 10, 0, "test"};
}

uint64_t trade_reports(const std::vector<ExecutionReport>& reports)
{
    uint64_t count = 0;
    for (const ExecutionReport& report : reports)
        if (report.exec_type == ExecType::Trade) ++count;
    return count;
}
}  // namespace

int main()
{
    // 1. No knobs: the gateway forwards the venue's reports unchanged, absorbs a second submission
    //    under a client id it is already holding, and counts a cancel that arrives after the order is
    //    gone instead of sending it to a venue that has nothing to cancel.
    {
        MatchingEngine venue;
        OrderGateway gateway(venue);
        std::vector<ExecutionReport> seen;
        gateway.set_report_callback([&seen](const ExecutionReport& report)
                                    { seen.push_back(report); });

        gateway.send_order(resting_buy(1, 10));
        assert(seen.size() == 1);
        assert(seen.back().exec_type == ExecType::Resting);
        assert(gateway.in_flight() == 1);
        assert(gateway.summary().submissions == 1);
        assert(gateway.summary().acks == 1);

        // The same client id again is a retry: the venue already has the order, so it never sees this.
        gateway.send_order(resting_buy(1, 11));
        assert(gateway.summary().submissions == 2);
        assert(gateway.summary().duplicates_suppressed == 1);
        assert(gateway.in_flight() == 1);
        assert(seen.size() == 1);

        gateway.cancel_order(1);
        assert(seen.back().exec_type == ExecType::Cancelled);
        assert(gateway.in_flight() == 0);

        // The order is gone, so the second cancel is counted and not forwarded.
        gateway.cancel_order(1);
        assert(gateway.summary().cancels == 2);
        assert(gateway.summary().cancels_after_fill == 1);
        assert(gateway.rejected_orders() == venue.rejected_orders());
    }

    // 2. The venue holds the order for 100 microseconds. A timeout of 50 fires first, the gateway sends
    //    the order again under the same id, and the venue refuses the copy as a duplicate. Two sends,
    //    one resting order, no trade: the position moves once.
    {
        MatchingEngine venue(PRICE_TICK_SIZE, FeeSchedule{},
                             MatchingEngineConfig{.order_latency_us = 100});
        OrderGateway gateway(venue, GatewayConfig{.ack_timeout_us = 50, .retry_on_timeout = true});
        std::vector<ExecutionReport> seen;
        gateway.set_report_callback([&seen](const ExecutionReport& report)
                                    { seen.push_back(report); });

        gateway.send_order(resting_buy(1, 1000));
        assert(seen.empty());  // still in the venue's in-flight queue
        assert(gateway.in_flight() == 1);

        // The clock has passed the deadline but not the venue's latency: the order is timed out and
        // retried, and the retry is queued behind the original.
        gateway.process_bbo(quote_at(1060));
        assert(gateway.summary().ack_timeouts == 1);
        assert(gateway.summary().retries == 1);
        assert(gateway.in_flight() == 1);

        // The original lands and answers the order.
        gateway.process_bbo(quote_at(1100));
        assert(seen.size() == 1);
        assert(seen.back().exec_type == ExecType::Resting);
        assert(gateway.summary().acks == 1);

        // The retry lands and is refused by the venue as a duplicate id. It produces no report, so the
        // strategy is told about one order and the venue holds one order.
        gateway.process_bbo(quote_at(1160));
        assert(seen.size() == 1);
        assert(trade_reports(seen) == 0);
        assert(venue.rejected_orders() == 1);
        assert(gateway.rejected_orders() == venue.rejected_orders());
        assert(gateway.in_flight() == 1);
    }

    return 0;
}
