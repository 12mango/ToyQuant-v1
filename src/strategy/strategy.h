#pragma once
#include <sstream>
#include <string>
#include <vector>

#include "common/types.h"
#include "exchange/execution_report.h"
#include "market/market_event.h"
#include "orderbook/l2_market_view.h"
#include "orderbook/orderbook.h"

struct StrategyOrder
{
    Side side = Side::Unknown;
    std::string symbol;
    double price = 0.0;
    uint64_t quantity = 0;
    uint64_t order_id = 0;

    StrategyOrder() = default;

    StrategyOrder(Side s, const std::string& sym, double p, uint64_t qty, uint64_t id)
        : side(s), symbol(sym), price(p), quantity(qty), order_id(id)
    {
    }
};

// What a passive fill at a given distance from the midpoint has actually cost, in ticks per unit, measured
// from our own resolved fills. It is the second term a quote has to cover — a fee is a schedule, but whether
// being passive at three ticks from the mid loses money is a fact about this window — and it can only come
// from the pipeline, because a strategy never sees the midpoint after a fill. Negative means the fills at that
// distance have lost money. `samples` is how many fills have resolved, which is what lets the gate wait until
// it knows something before it tightens.
struct MarkoutEstimate
{
    double buy_near_ticks{0.0};   // a passive buy within one tick of the midpoint
    double buy_far_ticks{0.0};    // a passive buy at least a tick away
    double sell_near_ticks{0.0};
    double sell_far_ticks{0.0};
    uint64_t samples{0};
};

// What a strategy knows about its own order flow. Execution quality is deliberately not here: the
// pipeline sees every report and every event, so it computes edge, markout, quote lifetime and
// inventory once, in ExecutionQualityMetrics, and prints them in [EXECUTION]. A second copy owned by
// the strategy could only disagree with it, and it did: [STRATEGY_METRICS] printed captured_edge=0
// and max_abs_inventory=0 beside an [EXECUTION] line that said 1.25 and 10 in the same run, because
// the L2 strategies never filled those fields and only the legacy L1 maker did, from its own mid.
struct StrategyMetrics
{
    uint64_t submitted_quantity{0};
    uint64_t filled_quantity{0};
    uint64_t fill_count{0};
    uint64_t cancel_count{0};
    uint64_t quote_count{0};
    double fees_paid{0.0};
    uint64_t price_refresh_count{0};
    uint64_t age_refresh_count{0};
    uint64_t risk_pause_count{0};
    // The decision layer's refusals, owned by the strategy because it is the thing that decided.
    uint64_t edge_gate_skips{0};
    uint64_t min_spread_skips{0};
    uint64_t buy_queue_consumed{0};
    uint64_t sell_queue_consumed{0};
    uint64_t buy_fill_count{0};
    uint64_t sell_fill_count{0};
    uint64_t buy_quote_count{0};
    uint64_t sell_quote_count{0};
    bool available{false};

    std::string to_log_string() const
    {
        const double fill_rate =
            submitted_quantity == 0
                ? 0.0
                : static_cast<double>(filled_quantity) / static_cast<double>(submitted_quantity);
        std::ostringstream stream;
        stream << "submitted_quantity=" << submitted_quantity
               << " filled_quantity=" << filled_quantity << " fill_rate=" << fill_rate
               << " fill_count=" << fill_count << " cancel_count=" << cancel_count
               << " quote_count=" << quote_count << " fees_paid=" << fees_paid
               << " price_refresh_count=" << price_refresh_count
               << " age_refresh_count=" << age_refresh_count
               << " risk_pause_count=" << risk_pause_count
               << " edge_gate_skips=" << edge_gate_skips
               << " min_spread_skips=" << min_spread_skips;
        stream << " buy_queue_consumed=" << buy_queue_consumed
               << " sell_queue_consumed=" << sell_queue_consumed
               << " buy_fill_count=" << buy_fill_count
               << " sell_fill_count=" << sell_fill_count
               << " buy_quote_count=" << buy_quote_count
               << " sell_quote_count=" << sell_quote_count;
        return stream.str();
    }
};

class Strategy
{
   public:
    virtual ~Strategy() = default;

    // Called on a top-of-book update; returns orders to submit.
    virtual std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                                      const TopOfBook& tob) = 0;

    virtual std::vector<StrategyOrder> on_l2_market_view(const L2MarketView& view)
    {
        return on_top_of_book(view.symbol, view.top);
    }

    // Called for venue trades before the next strategy decision.
    virtual void on_market_trade(const MarketTrade& trade)
    {
        (void)trade;
    }

    virtual void on_queue_activity(Side side, uint64_t consumed_quantity)
    {
        (void)side;
        (void)consumed_quantity;
    }

    // Called with the latest BBO when the coordinator has one for this symbol.
    virtual void on_market_trade(const MarketTrade& trade, const BboQuote* latest_bbo)
    {
        (void)latest_bbo;
        on_market_trade(trade);
    }

    // Called after an order receives its final ID and before it is sent to the engine.
    virtual void on_order_submitted(const StrategyOrder& order) = 0;

    // Returns working orders that must be cancelled before the next quote cycle.
    virtual std::vector<uint64_t> cancel_requests() = 0;

    virtual int64_t net_position() const = 0;
    virtual size_t working_order_count() const = 0;

    virtual StrategyMetrics metrics() const
    {
        return {};
    }

    // Called for trade, cancellation, resting, and fill reports.
    virtual void on_order_update(const ExecutionReport& rpt) = 0;

    // The running adverse-selection estimate, pushed by the pipeline as prints resolve. A strategy cannot
    // measure this itself: it never sees the midpoint after a fill, which is exactly what the number is. The
    // default returns nothing, so a strategy that does not ask for it pays nothing.
    virtual void on_markout_estimate(const MarkoutEstimate& estimate)
    {
        (void)estimate;
    }

    // Sides refused because the adverse-selection gate could not be covered, for a run that asked for it.
    // Reported by the pipeline in its [GATE] line rather than in StrategyMetrics, because a run that does
    // not ask for the gate must produce exactly the output it produced before the gate existed.
    virtual uint64_t markout_gate_skips() const
    {
        return 0;
    }

    // Orders sent to reduce a position that had grown past the band the run configured, and the quantity they
    // carried. Also reported in the [GATE] line, for the same reason.
    virtual uint64_t flatten_orders() const
    {
        return 0;
    }

    virtual uint64_t flatten_quantity() const
    {
        return 0;
    }
};
