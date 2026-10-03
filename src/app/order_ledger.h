#pragma once

#include <cstdint>
#include <fstream>
#include <string>
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
};
