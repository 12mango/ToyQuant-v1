// A broker has two halves to its contract and both are checked here: one with no faults configured is
// invisible, and a faulted one is visible in the two places that matter, its summary and the statement
// it keeps of what the venue executed. A fault injection whose faults cannot be seen is worse than none,
// because the run that carries it then looks exactly like a clean one.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "accounting/portfolio.h"
#include "accounting/reconciliation.h"
#include "exchange/matching_engine.h"
#include "exchange/paper_broker.h"

namespace
{
constexpr const char* kSymbol = "EURUSD";
constexpr double kPrice = 1.10000;
constexpr uint64_t kQuantity = 100;
constexpr uint64_t kTradeQuantity = 40;

// A maker rate that is not zero, so a fee is charged and a split fill's fee can be compared with the
// fee one print would have carried, rather than both sides being zero and agreeing.
FeeSchedule fee_schedule()
{
    return FeeSchedule{.maker_rate = 0.0001, .taker_rate = 0.0002, .quantity_scale = 1};
}

exchange::Order resting_order(uint64_t id, exchange::Side side, double price)
{
    return {id,       kSymbol,  side,                 exchange::OrderType::Limit, price,
            kQuantity, kQuantity, id,                "MarketMaker"};
}

exchange::Order resting_buy(uint64_t id)
{
    return resting_order(id, exchange::Side::Buy, kPrice);
}

// A limit order that rests and then an external sell that fills part of it: the smallest scenario in
// which a broker has anything to do.
void rest_then_trade(IMatchingEngine& engine)
{
    engine.send_order(resting_buy(1));
    engine.process_market_trade({2, kSymbol, kPrice, kTradeQuantity, Side::Sell, 0, "test"});
}

uint64_t reports_for(const std::vector<ExecutionReport>& reports, uint64_t order_id)
{
    return static_cast<uint64_t>(std::count_if(reports.begin(), reports.end(),
                                               [order_id](const ExecutionReport& report)
                                               { return report.order_id == order_id; }));
}

uint64_t reports_of(const std::vector<ExecutionReport>& reports, uint64_t order_id, ExecType type)
{
    return static_cast<uint64_t>(std::count_if(
        reports.begin(), reports.end(),
        [order_id, type](const ExecutionReport& report)
        { return report.order_id == order_id && report.exec_type == type; }));
}

// The index of the first report about an order, which is how the order of delivery is read back.
std::size_t first_report_index(const std::vector<ExecutionReport>& reports, uint64_t order_id)
{
    for (std::size_t index = 0; index < reports.size(); ++index)
        if (reports[index].order_id == order_id) return index;
    return reports.size();
}

// What the strategy's side received: exactly the reports a portfolio books.
struct BookedFills
{
    uint64_t count{0};
    uint64_t quantity{0};
    double fees{0.0};
};

BookedFills booked_fills(const std::vector<ExecutionReport>& reports)
{
    BookedFills booked;
    for (const ExecutionReport& report : reports)
    {
        if (report.exec_type != ExecType::Trade || report.owner != "MarketMaker") continue;
        ++booked.count;
        booked.quantity += report.quantity;
        booked.fees += report.fee;
    }
    return booked;
}

// The pipeline's own filter, so the reconciliation is checked against the accounting a run really has.
Portfolio book(const std::vector<ExecutionReport>& reports)
{
    Portfolio books(1, 1000.0);
    for (const ExecutionReport& report : reports)
    {
        if (report.owner != "MarketMaker") continue;
        books.apply(report);
    }
    return books;
}
}  // namespace

int main()
{
    // 1. With no faults configured the broker is a pass-through: the same reports in the same order and
    //    a statement that agrees with them, which is what makes a brokered run comparable to a direct one.
    {
        std::vector<ExecutionReport> direct;
        MatchingEngine plain_venue;
        plain_venue.set_report_callback([&direct](const ExecutionReport& report)
                                        { direct.push_back(report); });
        rest_then_trade(plain_venue);

        std::vector<ExecutionReport> brokered;
        MatchingEngine venue;
        PaperBroker broker(venue);
        broker.set_report_callback([&brokered](const ExecutionReport& report)
                                   { brokered.push_back(report); });
        rest_then_trade(broker);

        assert(direct.size() == brokered.size());
        for (std::size_t index = 0; index < direct.size(); ++index)
        {
            assert(direct[index].order_id == brokered[index].order_id);
            assert(direct[index].exec_type == brokered[index].exec_type);
            assert(direct[index].quantity == brokered[index].quantity);
            assert(direct[index].price == brokered[index].price);
            assert(direct[index].side == brokered[index].side);
            assert(direct[index].fee == brokered[index].fee);
        }

        const BrokerSummary& summary = broker.summary();
        assert(summary.orders_submitted == 1);
        assert(summary.orders_rejected == 0);
        assert(summary.fills_seen == 1);
        assert(summary.fills_split == 0 && summary.fills_dropped == 0);
        assert(summary.outages == 0 && summary.orders_held == 0);
        assert(broker.rejected_orders() == venue.rejected_orders());
        assert(broker.statement().executions == 1);
        assert(broker.statement().forwarded_fills == 1);
        assert(broker.statement().symbols.at(kSymbol).net_quantity ==
               static_cast<int64_t>(kTradeQuantity));
        assert(broker.statement().symbols.at(kSymbol).fill_count == 1);
    }

    // 2. Every second order is refused, and the refusal is where the pipeline looks for it: in
    //    rejected_orders(), the only signal a broker that emits no report can give.
    {
        MatchingEngine venue;
        PaperBroker broker(venue, BrokerFaults{.reject_every = 2});
        std::vector<ExecutionReport> seen;
        broker.set_report_callback([&seen](const ExecutionReport& report)
                                   { seen.push_back(report); });

        const uint64_t before = broker.rejected_orders();
        broker.send_order(resting_buy(1));
        assert(broker.rejected_orders() == before);
        broker.send_order(resting_buy(2));
        assert(broker.rejected_orders() == before + 1);

        assert(broker.summary().orders_submitted == 2);
        assert(broker.summary().orders_rejected == 1);
        // The venue never saw it and refused nothing of its own, which is what separates a broker's
        // refusal from the venue's.
        assert(reports_for(seen, 2) == 0);
        assert(reports_for(seen, 1) > 0);
        assert(venue.rejected_orders() == 0);
        assert(broker.statement().executions == 0);
    }

    // 3. A fill split in two: the position and the fee are what one print would have been, so only the
    //    report count changes, and the reconciliation still agrees.
    {
        std::vector<ExecutionReport> whole_reports;
        MatchingEngine plain_venue(0.00001, fee_schedule());
        plain_venue.set_report_callback([&whole_reports](const ExecutionReport& report)
                                        { whole_reports.push_back(report); });
        rest_then_trade(plain_venue);
        const BookedFills whole = booked_fills(whole_reports);
        assert(whole.count == 1);
        assert(whole.quantity == kTradeQuantity);
        assert(whole.fees > 0.0);

        std::vector<ExecutionReport> split_reports;
        MatchingEngine venue(0.00001, fee_schedule());
        PaperBroker broker(venue, BrokerFaults{.partial_every = 1});
        broker.set_report_callback([&split_reports](const ExecutionReport& report)
                                   { split_reports.push_back(report); });
        rest_then_trade(broker);

        const BookedFills split = booked_fills(split_reports);
        assert(split.count == 2);
        assert(split.quantity == whole.quantity);
        assert(std::abs(split.fees - whole.fees) < 1e-12);
        // One execution at the venue, two prints to the strategy.
        assert(broker.summary().fills_seen == 1);
        assert(broker.summary().fills_split == 1);
        assert(broker.statement().executions == 1);
        assert(broker.statement().forwarded_fills == 2);

        const ReconciliationSummary recon =
            reconcile_positions(broker.statement(), book(split_reports));
        assert(!recon.broken());
        assert(recon.symbols_checked == 1);
        assert(recon.report_count_difference == 0);
        assert(recon.quantity_breaks == 0);
        assert(recon.gross_quantity_break == 0);
        assert(recon.fees_match);
    }

    // 4. A lost execution report: the venue filled and the statement says so, and the strategy was never
    //    told. This is the break reconciliation exists to find, found in quantity and in fees.
    {
        MatchingEngine venue(0.00001, fee_schedule());
        PaperBroker broker(venue, BrokerFaults{.drop_every = 1});
        std::vector<ExecutionReport> seen;
        broker.set_report_callback([&seen](const ExecutionReport& report)
                                   { seen.push_back(report); });
        rest_then_trade(broker);

        assert(broker.summary().fills_seen == 1);
        assert(broker.summary().fills_dropped == 1);
        assert(broker.statement().executions == 1);
        assert(broker.statement().forwarded_fills == 0);
        assert(booked_fills(seen).count == 0);

        const ReconciliationSummary recon = reconcile_positions(broker.statement(), book(seen));
        assert(recon.broken());
        assert(recon.quantity_breaks == 1);
        assert(recon.max_quantity_break == static_cast<int64_t>(kTradeQuantity));
        assert(recon.net_quantity_break == static_cast<int64_t>(kTradeQuantity));
        assert(recon.gross_quantity_break == kTradeQuantity);
        assert(!recon.fees_match);
        const std::string line = recon.to_log_string();
        assert(line.find("status=break") != std::string::npos);
        assert(line.find("max_quantity_break=40") != std::string::npos);
    }

    // 5. A loss that nets out: a buy fill and a sell fill both withheld leave the position exactly where
    //    it was, and the two accounts still disagree. The gross column is the only one that catches it,
    //    which is why the reconciliation carries it next to the net.
    {
        MatchingEngine venue(0.00001, fee_schedule());
        PaperBroker broker(venue, BrokerFaults{.drop_every = 1});
        std::vector<ExecutionReport> seen;
        broker.set_report_callback([&seen](const ExecutionReport& report)
                                   { seen.push_back(report); });

        broker.send_order(resting_buy(1));
        broker.process_market_trade({2, kSymbol, kPrice, kTradeQuantity, Side::Sell, 0, "test"});
        // Away from the resting buy, so the sell rests instead of crossing it.
        broker.send_order(resting_order(3, exchange::Side::Sell, 1.20000));
        broker.process_market_trade({4, kSymbol, 1.20000, kTradeQuantity, Side::Buy, 0, "test"});

        assert(broker.summary().fills_seen == 2);
        assert(broker.summary().fills_dropped == 2);
        assert(broker.statement().symbols.at(kSymbol).net_quantity == 0);
        assert(broker.statement().symbols.at(kSymbol).filled_quantity == 2 * kTradeQuantity);

        const Portfolio booked = book(seen);
        assert(booked.positions().empty());
        const ReconciliationSummary recon = reconcile_positions(broker.statement(), booked);
        assert(recon.quantity_breaks == 0);
        assert(recon.net_quantity_break == 0);
        assert(recon.gross_quantity_break == 2 * kTradeQuantity);
        assert(recon.broken());
    }

    // 6. An outage: orders sent while the venue is unreachable are held and delivered in submission order
    //    once it is back, which is what an asynchronous submit looks like from the strategy's side.
    {
        MatchingEngine venue;
        PaperBroker broker(venue, BrokerFaults{.outage_after = 1, .outage_holds = 2});
        std::vector<ExecutionReport> seen;
        broker.set_report_callback([&seen](const ExecutionReport& report)
                                   { seen.push_back(report); });

        broker.send_order(resting_buy(1));
        assert(reports_for(seen, 1) > 0);
        broker.send_order(resting_buy(2));
        broker.send_order(resting_buy(3));
        assert(broker.held_orders() == 2);
        assert(reports_for(seen, 2) == 0);
        assert(reports_for(seen, 3) == 0);

        // The first order after the hold is the one that finds the venue back.
        broker.send_order(resting_buy(4));
        assert(broker.held_orders() == 0);
        assert(first_report_index(seen, 2) < first_report_index(seen, 3));
        assert(first_report_index(seen, 3) < first_report_index(seen, 4));
        assert(broker.summary().outages == 1);
        assert(broker.summary().orders_held == 2);
        assert(broker.summary().orders_released == 2);
        assert(broker.summary().orders_submitted == 4);
    }

    // 7. A cancel for an order the venue has not been given resolves against the hold rather than
    //    disappearing, and a hold of zero is a venue that never comes back.
    {
        MatchingEngine venue;
        PaperBroker broker(venue, BrokerFaults{.outage_after = 1});
        std::vector<ExecutionReport> seen;
        broker.set_report_callback([&seen](const ExecutionReport& report)
                                   { seen.push_back(report); });

        broker.send_order(resting_buy(1));
        broker.send_order(resting_buy(2));
        assert(broker.held_orders() == 1);
        broker.cancel_order(2);
        assert(broker.held_orders() == 0);
        assert(broker.summary().cancels_while_held == 1);
        // The order is gone for good, so the strategy is answered as if the venue had cancelled it: a
        // cancel that resolved silently left the strategy waiting for a confirmation.
        assert(reports_of(seen, 2, ExecType::Cancelled) == 1);
        assert(reports_for(seen, 2) == 1);

        broker.send_order(resting_buy(3));
        assert(broker.held_orders() == 1);
        assert(reports_for(seen, 3) == 0);

        // An order the venue does have is cancelled through it, so the hold swallows no cancels.
        broker.cancel_order(1);
        assert(reports_of(seen, 1, ExecType::Cancelled) == 1);
        assert(broker.summary().cancels_while_held == 1);
    }

    return 0;
}
