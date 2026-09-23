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

struct StrategyMetrics
{
    uint64_t submitted_quantity{0};
    uint64_t filled_quantity{0};
    uint64_t fill_count{0};
    uint64_t cancel_count{0};
    uint64_t quote_count{0};
    double fees_paid{0.0};
    double captured_edge{0.0};
    double adverse_selection{0.0};
    uint64_t markout_count{0};
    uint64_t total_quote_lifetime{0};
    uint64_t max_quote_lifetime{0};
    double average_abs_inventory{0.0};
    int64_t max_abs_inventory{0};
    uint64_t inventory_sign_changes{0};
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
               << " captured_edge=" << captured_edge << " adverse_selection=" << adverse_selection
               << " markout_count=" << markout_count
               << " total_quote_lifetime=" << total_quote_lifetime
               << " max_quote_lifetime=" << max_quote_lifetime
               << " avg_abs_inventory=" << average_abs_inventory
               << " max_abs_inventory=" << max_abs_inventory
               << " inventory_sign_changes=" << inventory_sign_changes;
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
};
