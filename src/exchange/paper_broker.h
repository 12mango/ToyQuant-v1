#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

#include "exchange/matching_engine.h"
#include "exchange/execution_report.h"
#include "exchange/order.h"

/*
-------------------------------------------
 PaperBroker
-------------------------------------------

The layer between a strategy and the venue, modelled as a decorator over the engine this project
already has. The pipeline knows only `IMatchingEngine`, so wrapping the engine is the whole change: the
strategy, the book and the accounting are untouched, and a broker with no faults configured forwards
every call and every report, which is what makes a run with it comparable to one without.

What a real broker does that a venue does not, and what each of these models:

  * **Refuses orders**, for reasons a strategy cannot see in the price: buying power, a price band, a
    duplicate client id. A refusal emits no report, which is exactly why `IMatchingEngine` carries a
    reject count for the pipeline to compare across a submission.
  * **Fills in pieces.** A venue fill of ten arrives as two prints of five, with the fee split the same
    way, so the position and the fee are what they would have been and only the report count changes.
  * **Loses a message.** The trade happened and the statement says so; the strategy never heard. This is
    the fault reconciliation exists to catch, and it is modelled as a report withheld on the way out
    rather than as a fill that did not happen.
  * **Goes away for a while.** Orders sent while the venue is unreachable are held and delivered in the
    order they were sent once it is back, which is what an asynchronous submit looks like from the
    strategy's side; a cancel for an order that is still held cancels it before it is ever sent.

Every fault is counted by number of orders or fills rather than drawn from a generator, so a faulted run
is reproducible from its command line alone. The counts are reported, and the broker keeps its own
statement of what the venue executed, which `reconcile_positions` compares against the portfolio.
-------------------------------------------
*/

// Zero everywhere, the default, is a broker with no faults: transparent, and the mode a run uses to
// check that wrapping the engine changed nothing.
struct BrokerFaults
{
    // Refuse every Nth order, counted over orders submitted. 0 refuses none.
    uint64_t reject_every{0};
    // Split every Nth of this strategy's fills into two prints. 0 splits none.
    uint64_t partial_every{0};
    // Withhold every Nth of this strategy's fills from the strategy while still recording it as
    // executed at the venue. 0 drops none.
    uint64_t drop_every{0};
    // Once this many orders have been accepted, the venue becomes unreachable until the next order
    // arrives after `outage_holds` more have been held. 0 never goes down.
    uint64_t outage_after{0};
    // How many orders the outage holds. 0 with `outage_after` set holds everything to the end.
    uint64_t outage_holds{0};
};

// The broker's own statement: what the venue executed, built from the reports as they arrive rather
// than from the portfolio, so the two are genuinely independent accounts of the same session. The two
// counts are kept apart on purpose: `executions` is what the venue did, `forwarded_fills` is what the
// strategy was told about, and the difference between them is a lost report.
struct BrokerStatement
{
    struct SymbolStatement
    {
        int64_t net_quantity{0};
        uint64_t filled_quantity{0};
        uint64_t fill_count{0};
        double fees{0.0};
    };

    std::unordered_map<std::string, SymbolStatement> symbols;
    uint64_t executions{0};
    uint64_t forwarded_fills{0};
    double fees{0.0};
};

struct BrokerSummary
{
    uint64_t orders_submitted{0};
    uint64_t orders_rejected{0};
    uint64_t fills_seen{0};
    uint64_t fills_split{0};
    uint64_t fills_dropped{0};
    uint64_t outages{0};
    uint64_t orders_held{0};
    uint64_t orders_released{0};
    uint64_t cancels_while_held{0};

    std::string to_log_string() const;
};

class PaperBroker : public IMatchingEngine
{
   public:
    // The venue callback is taken over here rather than in set_report_callback, so a caller that never
    // sets one still gets the statement built: the broker has to see every report anyway.
    PaperBroker(IMatchingEngine& venue, BrokerFaults faults = {});

    void send_order(const exchange::Order& order) override;
    void process_bbo(const BboQuote& quote) override;
    void process_l2_top(const BboQuote& quote) override;
    void process_market_trade(const MarketTrade& trade) override;
    void cancel_order(uint64_t order_id) override;
    void set_report_callback(ReportCallback callback) override;

    // Forwarded to the venue, plus this broker's own refusals: the pipeline decides whether an order
    // was accepted by watching this counter change across a submission, so a broker refusal has to be
    // in it and not only in the summary.
    uint64_t rejected_orders() const override;

    uint64_t queue_ahead_consumed() const override;
    uint64_t queue_ahead_consumed(Side side) const override;
    uint64_t queue_ahead_levels_cleared(Side side) const override;
    uint64_t queue_ahead_from_quantity_changes(Side side) const override;
    uint64_t queue_ahead_at(const std::string& symbol, Side side, double price) const override;

    const BrokerSummary& summary() const
    {
        return summary_;
    }

    const BrokerStatement& statement() const
    {
        return statement_;
    }

    // Orders the venue has not been given yet because it was unreachable when they were sent.
    std::size_t held_orders() const
    {
        return held_.size();
    }

   private:
    void on_venue_report(const ExecutionReport& report);
    void forward(const ExecutionReport& report);
    void record_execution(const ExecutionReport& report, uint64_t quantity, double fee);
    void release_held();

    IMatchingEngine& venue_;
    BrokerFaults faults_;
    ReportCallback consumer_;
    BrokerSummary summary_;
    BrokerStatement statement_;
    // Orders accepted while the venue was down, in submission order.
    std::deque<exchange::Order> held_;
    uint64_t accepted_orders_{0};
    uint64_t held_since_outage_{0};
    bool outage_active_{false};
    bool outage_seen_{false};
    uint64_t broker_rejects_{0};
};
