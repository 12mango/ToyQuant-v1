#pragma once

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app/run_summary.h"
#include "exchange/execution_report.h"
#include "exchange/order.h"
#include "strategy/strategy.h"

class IMatchingEngine;

// The record of the orders a run placed: the two CSV files it writes, and the trace of the one order a
// reader named.
//
// Both are per-order records rather than run totals, which is what keeps them out of the run's
// accounting: they answer "what happened to this order" where the accounting answers "what did the run
// do". The coordinator used to hold their state beside its counters and its gates, where a reader had to
// pick the two fields of twenty-one that belonged to an order.
//
// The row shapes are a documented data contract (data/README.md): an order row is
// `ts,symbol,side,price,quantity,order_id`, and a trade row adds the liquidity role and the fee. They
// live here rather than in the coordinator so that the file and the shape written into it are read
// together.
class OrderLedger
{
   public:
    // The state machine an audited order moves through. A terminal state closes the entry; the map holds
    // only open orders, so a report that arrives after one of them is an orphan rather than another
    // transition, which is exactly the duplicate/late-report case the counts below are for.
    enum class OrderState : uint8_t
    {
        Pending = 0,
        Resting,
        PartiallyFilled,
        Filled,
        Cancelled,
        Rejected
    };

    OrderLedger(std::ofstream& orders_out, std::ofstream& trades_out);

    // Builds the order the engine will see from the order the strategy asked for. It stays here because
    // the two sides name their side with different enumerations, and the translation is part of what the
    // ledger records.
    static exchange::Order to_exchange_order(const StrategyOrder& strategy_order, uint64_t ts,
                                             const std::string& owner);

    void write_order(uint64_t ts, const StrategyOrder& order);
    void write_trade(const ExecutionReport& report);

    // The queue trace. `set_trace_order` names the order to watch; 0, the default, traces nothing.
    // Nothing is traced until that order is submitted, because until then there is no price to look up
    // and a trace that guessed one would be worse than no trace.
    void set_trace_order(uint64_t order_id)
    {
        trace_order_id_ = order_id;
    }

    bool trace_requested() const
    {
        return trace_order_id_ != 0;
    }

    // The submit row: records the intent, with the queue the venue was displaying at that price before
    // our order joined it. The rest row that follows records the queue the model actually placed us
    // behind.
    void trace_submit(const StrategyOrder& order, uint64_t ts, IMatchingEngine& engine);

    // Every queue change between the submit and the end, called after an event that could have moved the
    // level. Consecutive samples that do not move are dropped, so a row means the queue actually moved.
    void trace_sample(const char* event, uint64_t ts, IMatchingEngine& engine);

    // The report that ends or continues the traced order. The clock is passed in rather than read from
    // the report: an order's own reports carry the order's timestamp, so a trace timed from report.ts
    // would print a cancel at the moment the order was submitted and the sequence would read backwards.
    // The row has to be timed by the event that caused it.
    void trace_report(const ExecutionReport& report, uint64_t current_ts, IMatchingEngine& engine);

    const std::vector<TraceRow>& trace_rows() const
    {
        return trace_rows_;
    }

    // --- an order's life ---
    //
    // The audit entry is opened the moment an order is submitted and closed by the report that ends it.
    // It is what turns a fill count into a fill rate: the queue the order joined behind, how close the
    // queue in front came while it waited, and how much of it was still there when the order left.
    void open_order(uint64_t order_id, uint64_t start_cycle, ExecutionQualityMetrics& quality);

    // The state of an order that is still open, or no value once it has reached a terminal state. It is
    // deliberately empty for a finished order: the audit entry is closed, and a caller that needs the
    // final state should read the terminal counts in the summary.
    std::optional<OrderState> order_state(uint64_t order_id) const;

    // The engine refused the order. Nothing rests and no report will ever arrive for it, so its entry goes
    // rather than staying to be counted forever as an order that never ended.
    void forget_order(uint64_t order_id);

    // The order rested: record the level, and the queue the model actually made it join behind. The level
    // exists only while one of our orders rests in it, which is why both ends of the measurement are taken
    // while the order is still there.
    void mark_resting(const ExecutionReport& report, IMatchingEngine& engine,
                      ExecutionQualityMetrics& quality);

    // The strategy asked to cancel. False when the cancel was already requested, so the caller counts one
    // request per order rather than one per cycle. The queue still in front is read here, before the engine
    // takes the order out: a reading taken after it left would report an empty queue for a level that was
    // not empty.
    bool begin_cancel(uint64_t order_id, IMatchingEngine& engine);

    // Samples the queue in front of every open order we are resting. The map holds open orders only, so
    // this is a handful of lookups per event. It is the difference between knowing one order's wait and
    // knowing the distribution of waits, and it runs whether or not a trace was asked for.
    void sample_resting(IMatchingEngine& engine);

    // Everything a report does to an order's own record, in the order the report arrives: the lifetime
    // when it ends, the fill and the commission accumulated against the floor, and the queue profile when
    // it leaves. Returns the commission shortfall the order owes under the per-order floor, which the
    // caller charges, because the ledger records and the account pays.
    //
    // `quantity_scale` converts executed quantity units to shares, which is what the broker's per-share
    // commission is charged on; it is passed in rather than owned here because it is a property of the
    // instrument, which the run's accounting holds.
    double on_report(const ExecutionReport& report, uint64_t quote_cycle, ExecutionQualityMetrics& quality,
                     double quantity_scale, double commission_per_unit,
                     double min_commission_per_order);

    // The order that has waited longest and is still in the book. Its lifetime is the one measure of
    // patience this project has, so it names the quote whose queue is worth tracing: it has survived every
    // requote the run made.
    // (id, cycles) of the order that has waited longest and is still in the book.
    std::pair<uint64_t, uint64_t> longest_working_order(uint64_t quote_cycle) const;

    uint64_t first_fill_order_id() const
    {
        return first_fill_order_id_;
    }

    uint64_t longest_wait_filled_order_id() const
    {
        return longest_wait_filled_order_id_;
    }

    uint64_t longest_wait_filled_cycles() const
    {
        return longest_wait_filled_cycles_;
    }

    uint64_t queue_profile_orders() const
    {
        return queue_profile_orders_;
    }

    uint64_t queue_zero_orders() const
    {
        return queue_zero_orders_;
    }

    const std::vector<double>& queue_min_fractions() const
    {
        return queue_min_fractions_;
    }

    const std::vector<double>& queue_end_fractions() const
    {
        return queue_end_fractions_;
    }

   private:
    std::ofstream& orders_out_;
    std::ofstream& trades_out_;

    uint64_t trace_order_id_{0};
    bool trace_level_known_{false};
    std::string trace_symbol_;
    Side trace_side_{Side::Unknown};
    double trace_price_{0.0};
    uint64_t trace_last_queue_{0};
    std::vector<TraceRow> trace_rows_;

    // The per-order audit entry. Every member is written before it is read: the submission opens it, the
    // resting report fills in the level, the cancel request reads the queue at the end, and the report
    // that ends the order closes it. The one that is not obvious is `profiling`: the level belongs to our
    // order while it rests, so the queue behind it is measured when the order arrives and again before any
    // cancel and never after it leaves, because the engine erases a level once no order rests in it and a
    // reading taken then would report an empty queue for a level that was not empty.
    struct OrderAudit
    {
        OrderState state{OrderState::Pending};
        uint64_t start_cycle{0};
        bool filled{false};
        // What the order filled in total, which is not the same quantity as any single fill. A share
        // commission is charged per share, but a broker's minimum is charged per order and a modification
        // makes a new order, so the quantity that decides a fee is the size of the order instance that
        // filled. It is accumulated here and bucketed when the order ends.
        uint64_t filled_quantity{0};
        // The broker's own share commission charged to this order so far, which is what its floor is
        // measured against when the order ends. It is not `fee`: the venue's rate and the fees on a sale
        // are not what a commission floor applies to.
        double commission{0.0};
        bool profiling{false};
        Side side{Side::Unknown};
        // Braced like every other member, because GCC's -Wmissing-field-initializers exempts members that
        // have a default initializer and reports the one that does not.
        std::string symbol{};
        double price{0.0};
        uint64_t queue_at_rest{0};
        uint64_t min_queue_ahead{0};
        uint64_t queue_at_end{0};
    };
    // The one place a transition is accepted or refused. It updates the state and counts a refusal, so a
    // duplicate or out-of-order report is visible instead of silently moving an order backwards.
    static bool transition(OrderState& state, OrderState next, ExecutionQualityMetrics& quality);

    std::unordered_map<uint64_t, OrderAudit> order_audit_;
    std::unordered_map<uint64_t, uint64_t> order_start_cycles_;
    std::unordered_set<uint64_t> pending_cancel_orders_;
    // One entry per order that rested and died, holding the two ratios worth aggregating: how close the
    // queue in front came, and how much of it was still there when the order left.
    std::vector<double> queue_min_fractions_;
    std::vector<double> queue_end_fractions_;
    uint64_t queue_profile_orders_{0};
    uint64_t queue_zero_orders_{0};
    // Which orders turned out to be worth tracing. A reader cannot know an id before the run, and the
    // first few orders are hostile samples: they die in the same millisecond they arrive. These are the
    // orders that actually waited, so a second run can name one of them.
    uint64_t first_fill_order_id_{0};
    uint64_t longest_wait_filled_order_id_{0};
    uint64_t longest_wait_filled_cycles_{0};
};
