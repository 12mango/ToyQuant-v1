#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "exchange/execution_report.h"
#include "exchange/matching_engine.h"
#include "exchange/order.h"

/*
-------------------------------------------
 OrderGateway
-------------------------------------------

A strategy's `send_order` is not the same moment as the venue accepting the order. The engine's
`send_order` is synchronous: by the time it returns, the order is resting, filled or refused. A real
client sits on a socket where the two moments are separate, and everything that makes an order gateway
different lives in that gap: an order that was sent but not answered, a retry that must not become a
second position, and a cancel that arrives after the order is already gone.

This is the same decorator shape as `PaperBroker`: the pipeline knows only `IMatchingEngine`, so
wrapping the venue is the whole change, and with every knob at zero it forwards every call and every
report. What it adds:

  * **The client order id is the order's id.** Every submission is entered in a table under that id
    before the venue is called, and a second submission under an id already in the table is absorbed
    here. That is idempotency from the client's side: a retry the strategy did not ask for cannot
    become a second position.
  * **An answer timeout.** An order the venue has not said anything about within `ack_timeout_us` of
    the market's own clock is counted as a timeout. With `retry_on_timeout` the order is sent again
    under the same id; the venue already refuses a duplicate id, so the retry cannot fill twice.
  * **A cancel for an order that has already finished** is counted rather than forwarded, because the
    venue has nothing left to cancel and silently ignoring it hides a strategy that is quoting stale
    state.

The clock is the market's, not the wall clock. A timestamp from the feed is the only clock a replay and
a live session share, and a timeout that means one thing in one and something else in the other is not
a measurement.
-------------------------------------------
*/

struct GatewayConfig
{
    // No answer about an order within this many microseconds of market time is a timeout. 0, the
    // default, counts none, which is the pass-through configuration.
    uint64_t ack_timeout_us{0};
    // Send a timed-out order again under the same client id. Off, the default. The venue refuses the
    // duplicate id, so this is the client half of an idempotent submit rather than a second order.
    bool retry_on_timeout{false};
};

struct GatewaySummary
{
    uint64_t submissions{0};
    // A submission under a client id the gateway is already holding: a retry, absorbed before the venue.
    uint64_t duplicates_suppressed{0};
    // Orders the venue said something about: a rest, a fill or a cancel. The first report of any kind is
    // the answer, which is why a marketable order that never rests still counts here.
    uint64_t acks{0};
    uint64_t ack_timeouts{0};
    uint64_t retries{0};
    // Retries the venue refused, counted when the refusal is visible at the moment of the retry. A
    // duplicate delivered later, from the venue's own in-flight queue, shows up in `venue_rejects`
    // instead.
    uint64_t retry_rejections{0};
    // Submissions the venue refused synchronously. The pipeline sees the same refusal through
    // `rejected_orders()`; the gateway counts it so the table it holds is cleaned up and the reason is
    // visible beside the timeouts.
    uint64_t venue_rejects{0};
    uint64_t cancels{0};
    // A cancel for an order that is no longer in the gateway's table, because it filled or was
    // cancelled already.
    uint64_t cancels_after_fill{0};

    std::string to_log_string() const;
};

class OrderGateway : public IMatchingEngine
{
   public:
    // The venue's callback is taken over here, as the broker does, because the gateway has to see every
    // report to know when an order has been answered.
    explicit OrderGateway(IMatchingEngine& venue, GatewayConfig config = {});

    void send_order(const exchange::Order& order) override;
    void process_bbo(const BboQuote& quote) override;
    void process_l2_top(const BboQuote& quote) override;
    void process_market_trade(const MarketTrade& trade) override;
    void cancel_order(uint64_t order_id) override;
    void set_report_callback(ReportCallback callback) override;

    // Forwarded: a refusal is the venue's answer, and the pipeline watches this counter change across
    // a submission. The gateway does not add to it, because a duplicate it absorbs was a retry of an
    // order the venue already has rather than a refusal of the strategy's order.
    uint64_t rejected_orders() const override
    {
        return venue_.rejected_orders();
    }

    uint64_t queue_ahead_consumed() const override
    {
        return venue_.queue_ahead_consumed();
    }
    uint64_t queue_ahead_consumed(Side side) const override
    {
        return venue_.queue_ahead_consumed(side);
    }
    uint64_t queue_ahead_levels_cleared(Side side) const override
    {
        return venue_.queue_ahead_levels_cleared(side);
    }
    uint64_t queue_ahead_from_quantity_changes(Side side) const override
    {
        return venue_.queue_ahead_from_quantity_changes(side);
    }
    uint64_t queue_ahead_at(const std::string& symbol, Side side, double price) const override
    {
        return venue_.queue_ahead_at(symbol, side, price);
    }

    const GatewaySummary& summary() const
    {
        return summary_;
    }

    // Orders the venue has not answered yet, which is the table a timeout sweeps.
    std::size_t in_flight() const
    {
        return in_flight_.size();
    }

   private:
    struct Tracked
    {
        exchange::Order order;
        uint64_t submitted_ts{0};
        // What the venue says is left of the order. Kept here because a Trade report carries what it
        // executed rather than what remains, and the table has to know when the order is finished.
        uint64_t remaining{0};
        bool answered{false};
        bool timed_out{false};
        bool retried{false};
    };

    void on_venue_report(const ExecutionReport& report);
    void forward(const ExecutionReport& report);
    // Called after a market event has been given to the venue, so an answer carried by that event is
    // applied before an order is called late.
    void sweep_timeouts(uint64_t now_ts);

    IMatchingEngine& venue_;
    GatewayConfig config_;
    ReportCallback consumer_;
    GatewaySummary summary_;
    std::unordered_map<uint64_t, Tracked> in_flight_;
};
