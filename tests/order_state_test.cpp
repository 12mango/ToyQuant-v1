// The order-state contract this test pins: a venue or broker refusal is a terminal Rejected report, it
// releases the quantity the position gate reserved, and it removes the order from the strategy's open set.
// The second submission is the assertion that matters: with a position limit of ten, a leaked reservation
// from the first (rejected) order would make the second order project twenty and be refused by the gate.
#include <cassert>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "accounting/portfolio.h"
#include "app/order_ledger.h"
#include "app/pipeline.h"
#include "exchange/matching_engine.h"
#include "lines/paper_broker.h"
#include "support/test_support.h"

namespace
{
class ProbeStrategy : public Strategy
{
   public:
    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& top) override
    {
        return {StrategyOrder(Side::Buy, symbol, top.ask_price, 10, 0)};
    }

    std::vector<StrategyOrder> on_l2_market_view(const L2MarketView& view) override
    {
        return {StrategyOrder(Side::Buy, view.symbol, view.top.ask_price, 10, 0)};
    }

    void on_order_submitted(const StrategyOrder& order) override
    {
        open_.insert(order.order_id);
    }

    std::vector<uint64_t> cancel_requests() override
    {
        return {};
    }

    int64_t net_position() const override
    {
        return 0;
    }

    size_t working_order_count() const override
    {
        return open_.size();
    }

    void on_order_update(const ExecutionReport& report) override
    {
        if (!is_terminal(report.exec_type)) return;
        open_.erase(report.order_id);
        if (report.exec_type == ExecType::Rejected) ++rejected_;
    }

    StrategyMetrics metrics() const override
    {
        return {};
    }

    uint64_t rejected() const
    {
        return rejected_;
    }

   private:
    std::unordered_set<uint64_t> open_;
    uint64_t rejected_{0};
};
}  // namespace

int main()
{
    // The two terminal states the rest of the code now shares.
    assert(is_terminal(ExecType::Filled));
    assert(is_terminal(ExecType::Cancelled));
    assert(is_terminal(ExecType::Rejected));
    assert(!is_terminal(ExecType::Trade));
    assert(!is_terminal(ExecType::Resting));

    test_support::TempFile orders_file("order_state_orders.csv");
    test_support::TempFile trades_file("order_state_trades.csv");
    std::ofstream orders = orders_file.open();
    std::ofstream trades = trades_file.open();

    ProbeStrategy strategy;
    MatchingEngine venue;
    PaperBroker broker(venue, BrokerFaults{.reject_every = 1});
    Portfolio portfolio;
    Pipeline pipeline(orders, trades, strategy, broker, portfolio);
    // Ten is exactly one order: a leaked pending quantity would refuse the second one.
    pipeline.set_position_limit(10);

    const L2MarketView view{.symbol = "TEST",
                            .ts = 1,
                            .top = TopOfBook{100.0, 100, 101.0, 100},
                            .micro_price = 100.5};
    pipeline.process_l2_market_view(view);

    auto summary = pipeline.summary();
    assert(summary.submitted_orders == 1);
    assert(summary.working_orders == 0);
    assert(summary.execution_quality.audited_rejected_orders == 1);
    assert(strategy.rejected() == 1);
    assert(broker.summary().orders_rejected == 1);
    assert(summary.to_log_string().find("audited_rejected_orders=1") != std::string::npos);

    pipeline.process_l2_market_view(view);
    summary = pipeline.summary();
    // The first order's reservation went back on the Rejected report, so the second one is submitted.
    assert(summary.submitted_orders == 2);
    assert(summary.working_orders == 0);
    assert(summary.execution_quality.audited_rejected_orders == 2);
    assert(strategy.rejected() == 2);
    assert(broker.summary().orders_rejected == 2);
    assert(summary.strategy_position_mismatches == 0);

    // The state machine itself: Pending -> Resting -> PartiallyFilled -> Filled, with an out-of-order
    // Resting counted rather than allowed, and a duplicate terminal report counted as an orphan.
    test_support::TempFile ledger_orders_file("order_state_ledger_orders.csv");
    test_support::TempFile ledger_trades_file("order_state_ledger_trades.csv");
    std::ofstream ledger_orders = ledger_orders_file.open();
    std::ofstream ledger_trades = ledger_trades_file.open();
    OrderLedger ledger(ledger_orders, ledger_trades);
    ExecutionQualityMetrics quality;
    MatchingEngine queue_engine;

    ledger.open_order(100, 0, quality);
    assert(ledger.order_state(100) == OrderLedger::OrderState::Pending);

    const ExecutionReport resting{.order_id = 100,
                                  .side = exchange::Side::Buy,
                                  .exec_type = ExecType::Resting,
                                  .symbol = "TEST",
                                  .price = 100.0,
                                  .quantity = 10,
                                  .ts = 1,
                                  .owner = "MarketMaker"};
    ledger.mark_resting(resting, queue_engine, quality);
    assert(ledger.order_state(100) == OrderLedger::OrderState::Resting);

    const ExecutionReport partial{.order_id = 100,
                                  .side = exchange::Side::Buy,
                                  .exec_type = ExecType::Trade,
                                  .symbol = "TEST",
                                  .price = 100.0,
                                  .quantity = 4,
                                  .ts = 2,
                                  .owner = "MarketMaker"};
    ledger.on_report(partial, 0, quality, 1, 0.0, 0.0);
    assert(ledger.order_state(100) == OrderLedger::OrderState::PartiallyFilled);

    // A second Resting report cannot move a partially filled order backwards.
    ledger.mark_resting(resting, queue_engine, quality);
    assert(quality.invalid_order_transitions == 1);
    assert(ledger.order_state(100) == OrderLedger::OrderState::PartiallyFilled);

    const ExecutionReport filled{.order_id = 100,
                                 .side = exchange::Side::Buy,
                                 .exec_type = ExecType::Filled,
                                 .symbol = "TEST",
                                 .price = 100.0,
                                 .quantity = 6,
                                 .ts = 3,
                                 .owner = "MarketMaker"};
    ledger.on_report(filled, 0, quality, 1, 0.0, 0.0);
    assert(!ledger.order_state(100).has_value());
    assert(quality.audited_filled_orders == 1);
    assert(quality.invalid_order_transitions == 1);

    // The audit is closed, so the same terminal report again is an orphan, not a second transition.
    ledger.on_report(filled, 0, quality, 1, 0.0, 0.0);
    assert(quality.orphan_order_reports == 1);

    return 0;
}
