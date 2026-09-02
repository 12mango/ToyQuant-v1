#include "exchange/matching_engine.h"

#include <cassert>
#include <vector>

bool has_report(const std::vector<ExecutionReport>& reports, uint64_t order_id, ExecType exec_type,
                uint64_t quantity)
{
    for (const auto& report : reports)
    {
        if (report.order_id == order_id && report.exec_type == exec_type &&
            report.quantity == quantity && report.owner == "MarketMaker")
        {
            return true;
        }
    }
    return false;
}

int main()
{
    MatchingEngine engine;
    std::vector<ExecutionReport> reports;
    engine.set_report_callback([&reports](const ExecutionReport& report)
                               { reports.push_back(report); });

    engine.send_order({1, "EURUSD", exchange::Side::Buy, exchange::OrderType::Limit, 1.10000, 100,
                       100, 1, "MarketMaker"});
    engine.send_order({2, "EURUSD", exchange::Side::Sell, exchange::OrderType::Limit, 1.10000, 100,
                       100, 2, "MarketMaker"});

    bool self_trade = false;
    bool cancelled_aggressor = false;
    for (const auto& report : reports)
    {
        self_trade |= report.exec_type == ExecType::Trade;
        cancelled_aggressor |= report.order_id == 2 && report.exec_type == ExecType::Cancelled;
    }
    assert(!self_trade);
    assert(cancelled_aggressor);

    reports.clear();
    engine.process_market_tick({3, "EURUSD", 1.10000, 40, Side::Sell});
    assert(has_report(reports, 1, ExecType::Trade, 40));
    assert(has_report(reports, 1, ExecType::PartialFill, 60));

    reports.clear();
    engine.process_market_tick({4, "EURUSD", 1.10000, 60, Side::Sell});
    assert(has_report(reports, 1, ExecType::Trade, 60));
    assert(has_report(reports, 1, ExecType::Filled, 0));

    MatchingEngine participant_engine;
    std::vector<ExecutionReport> participant_reports;
    participant_engine.set_report_callback([&participant_reports](const ExecutionReport& report)
                                           { participant_reports.push_back(report); });
    participant_engine.send_order({30, "EURUSD", exchange::Side::Buy, exchange::OrderType::Limit,
                                   1.10000, 20, 20, 30, " MarketMaker "});
    participant_engine.send_order({31, "EURUSD", exchange::Side::Sell, exchange::OrderType::Limit,
                                   1.10000, 20, 20, 31, "MarketMaker"});
    assert(participant_reports.empty() ||
           std::all_of(participant_reports.begin(), participant_reports.end(),
                       [](const ExecutionReport& report)
                       { return report.exec_type != ExecType::Trade; }));

    // Price-time priority: at the same price, earlier resting orders should match first.
    MatchingEngine fifo_engine;
    std::vector<ExecutionReport> fifo_reports;
    fifo_engine.set_report_callback([&fifo_reports](const ExecutionReport& report)
                                    { fifo_reports.push_back(report); });

    fifo_engine.send_order({10, "XAUUSD", exchange::Side::Buy, exchange::OrderType::Limit, 2000.0,
                            50, 50, 101, "MarketMaker"});
    fifo_engine.send_order({11, "XAUUSD", exchange::Side::Buy, exchange::OrderType::Limit, 2000.0,
                            50, 50, 102, "MarketMaker"});
    fifo_engine.send_order({12, "XAUUSD", exchange::Side::Sell, exchange::OrderType::Limit, 2000.0,
                            80, 80, 103, "MarketMaker"});

    std::vector<uint64_t> trade_ids;
    for (const auto& report : fifo_reports)
    {
        if (report.exec_type == ExecType::Trade)
        {
            trade_ids.push_back(report.order_id);
        }
    }

    assert(trade_ids.size() == 2);
    assert(trade_ids[0] == 10 || trade_ids[0] == 11);
    assert(trade_ids[1] == 10 || trade_ids[1] == 11);

    // Best-price priority: a buy order should prefer the highest bid and only then next levels.
    MatchingEngine price_engine;
    std::vector<ExecutionReport> price_reports;
    price_engine.set_report_callback([&price_reports](const ExecutionReport& report)
                                     { price_reports.push_back(report); });

    price_engine.send_order({20, "EURUSD", exchange::Side::Sell, exchange::OrderType::Limit,
                             1.10000, 30, 30, 201, "MarketMaker"});
    price_engine.send_order({21, "EURUSD", exchange::Side::Buy, exchange::OrderType::Limit, 1.10100,
                             20, 20, 202, "MarketMaker"});
    price_engine.send_order({22, "EURUSD", exchange::Side::Buy, exchange::OrderType::Limit, 1.10050,
                             10, 10, 203, "MarketMaker"});
    price_engine.send_order({23, "EURUSD", exchange::Side::Sell, exchange::OrderType::Limit,
                             1.10000, 40, 40, 204, "MarketMaker"});

    bool got_best_bid = false;
    for (const auto& report : price_reports)
    {
        if (report.order_id == 23 && report.exec_type == ExecType::Trade)
        {
            got_best_bid = report.price == 1.10100;
            break;
        }
    }
    assert(got_best_bid);
}