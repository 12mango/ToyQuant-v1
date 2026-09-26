#include "exchange/matching_engine.h"

#include <algorithm>
#include <cassert>
#include <cmath>
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
    engine.process_market_trade({3, "EURUSD", 1.10000, 40, Side::Sell, 0, "test"});
    assert(has_report(reports, 1, ExecType::Trade, 40));
    assert(has_report(reports, 1, ExecType::PartialFill, 60));

    reports.clear();
    engine.process_market_trade({4, "EURUSD", 1.10000, 60, Side::Sell, 0, "test"});
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
                            50, 50, 101, "LiquidityProvider"});
    fifo_engine.send_order({11, "XAUUSD", exchange::Side::Buy, exchange::OrderType::Limit, 2000.0,
                            50, 50, 102, "LiquidityProvider"});
    fifo_engine.send_order({12, "XAUUSD", exchange::Side::Sell, exchange::OrderType::Limit, 2000.0,
                            80, 80, 103, "MarketMaker"});

    std::vector<uint64_t> trade_ids;
    for (const auto& report : fifo_reports)
    {
        if (report.exec_type == ExecType::Trade && report.order_id != 12)
        {
            trade_ids.push_back(report.order_id);
        }
    }

    assert(trade_ids.size() == 2);
    assert(trade_ids[0] == 10);
    assert(trade_ids[1] == 11);

    // Best-price priority: a buy order should prefer the highest bid and only then next levels.
    MatchingEngine price_engine;
    std::vector<ExecutionReport> price_reports;
    price_engine.set_report_callback([&price_reports](const ExecutionReport& report)
                                     { price_reports.push_back(report); });

    price_engine.send_order({21, "EURUSD", exchange::Side::Buy, exchange::OrderType::Limit, 1.10100,
                             20, 20, 202, "LiquidityProvider"});
    price_engine.send_order({22, "EURUSD", exchange::Side::Buy, exchange::OrderType::Limit, 1.10050,
                             10, 10, 203, "LiquidityProvider"});
    price_engine.send_order({23, "EURUSD", exchange::Side::Sell, exchange::OrderType::Limit,
                             1.10000, 40, 40, 204, "MarketMaker"});

    bool got_best_bid = false;
    for (const auto& report : price_reports)
    {
        if (report.order_id == 23 && report.exec_type == ExecType::Trade)
        {
            got_best_bid = std::abs(report.price - 1.10100) < 1e-12;
            break;
        }
    }
    assert(got_best_bid);

    // Equivalent floating-point expressions must select the same integer price level.
    MatchingEngine tick_engine;
    std::vector<ExecutionReport> tick_reports;
    tick_engine.set_report_callback([&tick_reports](const ExecutionReport& report)
                                    { tick_reports.push_back(report); });
    tick_engine.send_order({40, "TEST", exchange::Side::Buy, exchange::OrderType::Limit, 0.1 + 0.2,
                            10, 10, 1, "LiquidityProvider"});
    tick_engine.process_market_trade({2, "TEST", 0.3, 10, Side::Sell, 0, "test"});
    assert(std::any_of(tick_reports.begin(), tick_reports.end(),
                       [](const ExecutionReport& report)
                       {
                           return report.order_id == 40 && report.exec_type == ExecType::Trade &&
                                  report.quantity == 10;
                       }));

    MatchingEngine bbo_engine(nullptr, 0.1);
    std::vector<ExecutionReport> bbo_reports;
    bbo_engine.set_report_callback([&bbo_reports](const ExecutionReport& report)
                                   { bbo_reports.push_back(report); });
    bbo_engine.process_bbo({10, "BTCUSDT", 100.0, 50, 100.1, 40, 1, "test"});
    bbo_engine.send_order({50, "BTCUSDT", exchange::Side::Buy, exchange::OrderType::Limit, 100.2,
                           70, 70, 11, "MarketMaker"});
    assert(has_report(bbo_reports, 50, ExecType::Trade, 40));
    assert(has_report(bbo_reports, 50, ExecType::PartialFill, 30));
    assert(has_report(bbo_reports, 50, ExecType::Resting, 30));

    bbo_reports.clear();
    bbo_engine.send_order({51, "BTCUSDT", exchange::Side::Buy, exchange::OrderType::Limit, 100.2,
                           10, 10, 12, "OtherParticipant"});
    assert(std::none_of(bbo_reports.begin(), bbo_reports.end(), [](const ExecutionReport& report)
                        { return report.exec_type == ExecType::Trade; }));

    bbo_reports.clear();
    bbo_engine.process_market_trade({13, "BTCUSDT", 100.2, 30, Side::Sell, 0, "test"});
    assert(has_report(bbo_reports, 50, ExecType::Trade, 30));
    assert(has_report(bbo_reports, 50, ExecType::Filled, 0));

    MatchingEngine queued_engine(nullptr, 0.1);
    std::vector<ExecutionReport> queued_reports;
    queued_engine.set_report_callback([&queued_reports](const ExecutionReport& report)
                                      { queued_reports.push_back(report); });
    queued_engine.process_l2_top({20, "BTCUSDT", 100.0, 50, 100.1, 40, 2, "test"});
    queued_engine.send_order({52, "BTCUSDT", exchange::Side::Buy,
                              exchange::OrderType::Limit, 100.0, 20, 20, 21,
                              "MarketMaker"});
    queued_reports.clear();
    queued_engine.process_market_trade({22, "BTCUSDT", 100.0, 40, Side::Sell, 0, "test"});
    assert(!has_report(queued_reports, 52, ExecType::Trade, 20));
    assert(queued_engine.queue_ahead_consumed(Side::Buy) == 40);
    assert(queued_engine.queue_ahead_levels_cleared(Side::Buy) == 0);
    queued_reports.clear();
    queued_engine.process_market_trade({23, "BTCUSDT", 100.0, 15, Side::Sell, 0, "test"});
    assert(has_report(queued_reports, 52, ExecType::Trade, 5));
    assert(has_report(queued_reports, 52, ExecType::PartialFill, 15));
    assert(queued_engine.queue_ahead_consumed(Side::Buy) == 50);
    assert(queued_engine.queue_ahead_levels_cleared(Side::Buy) == 1);

    MatchingEngine conservative_queue_engine(nullptr, 0.1, {}, 0, QueueModel::Conservative);
    std::vector<ExecutionReport> conservative_queue_reports;
    conservative_queue_engine.set_report_callback(
        [&conservative_queue_reports](const ExecutionReport& report)
        { conservative_queue_reports.push_back(report); });
    conservative_queue_engine.process_l2_top(
        {24, "CONSERVATIVE", 100.0, 50, 100.1, 40, 2, "test"});
    conservative_queue_engine.send_order({80, "CONSERVATIVE", exchange::Side::Buy,
                                          exchange::OrderType::Limit, 100.0, 10, 10, 25,
                                          "MarketMaker"});
    conservative_queue_engine.process_l2_top(
        {26, "CONSERVATIVE", 100.0, 10, 100.1, 40, 3, "test"});
    conservative_queue_reports.clear();
    conservative_queue_engine.process_market_trade(
        {27, "CONSERVATIVE", 100.0, 10, Side::Sell, 0, "test"});
    assert(!has_report(conservative_queue_reports, 80, ExecType::Trade, 10));
    assert(conservative_queue_engine.queue_ahead_from_quantity_changes(Side::Buy) == 0);

    MatchingEngine optimistic_queue_engine(nullptr, 0.1, {}, 0, QueueModel::Optimistic);
    std::vector<ExecutionReport> optimistic_queue_reports;
    optimistic_queue_engine.set_report_callback(
        [&optimistic_queue_reports](const ExecutionReport& report)
        { optimistic_queue_reports.push_back(report); });
    optimistic_queue_engine.process_l2_top(
        {30, "OPTIMISTIC", 100.0, 50, 100.1, 40, 2, "test"});
    optimistic_queue_engine.send_order({81, "OPTIMISTIC", exchange::Side::Buy,
                                        exchange::OrderType::Limit, 100.0, 10, 10, 31,
                                        "MarketMaker"});
    optimistic_queue_engine.process_l2_top(
        {32, "OPTIMISTIC", 100.0, 0, 100.1, 40, 3, "test"});
    optimistic_queue_engine.process_market_trade(
        {33, "OPTIMISTIC", 100.0, 10, Side::Sell, 0, "test"});
    assert(has_report(optimistic_queue_reports, 81, ExecType::Trade, 10));
    assert(optimistic_queue_engine.queue_ahead_from_quantity_changes(Side::Buy) == 50);

    MatchingEngine heuristic_queue_engine(nullptr, 0.1, {}, 0, QueueModel::Heuristic, 0.5);
    heuristic_queue_engine.process_l2_top({34, "HEURISTIC", 100.0, 50, 100.1, 40, 2, "test"});
    heuristic_queue_engine.send_order({82, "HEURISTIC", exchange::Side::Buy,
                                       exchange::OrderType::Limit, 100.0, 10, 10, 35,
                                       "MarketMaker"});
    heuristic_queue_engine.process_l2_top({36, "HEURISTIC", 100.0, 0, 100.1, 40, 3, "test"});
    assert(heuristic_queue_engine.queue_ahead_from_quantity_changes(Side::Buy) <= 13);

    queued_reports.clear();
    queued_engine.cancel_order(52);
    queued_engine.send_order({53, "BTCUSDT", exchange::Side::Buy,
                              exchange::OrderType::Limit, 100.0, 10, 10, 24,
                              "MarketMaker"});
    queued_reports.clear();
    queued_engine.process_market_trade({25, "BTCUSDT", 100.0, 1, Side::Sell, 0, "test"});
    assert(has_report(queued_reports, 53, ExecType::Trade, 1));
    assert(has_report(queued_reports, 53, ExecType::PartialFill, 9));

    MatchingEngine delayed_cancel_engine(nullptr, 0.1, {}, 1);
    std::vector<ExecutionReport> delayed_cancel_reports;
    delayed_cancel_engine.set_report_callback(
        [&delayed_cancel_reports](const ExecutionReport& report)
        { delayed_cancel_reports.push_back(report); });
    delayed_cancel_engine.process_bbo({30, "DELAY", 100.0, 10, 100.1, 10, 1, "test"});
    delayed_cancel_engine.send_order({70, "DELAY", exchange::Side::Buy,
                                      exchange::OrderType::Limit, 100.0, 10, 10, 31,
                                      "MarketMaker"});
    delayed_cancel_engine.cancel_order(70);
    delayed_cancel_engine.cancel_order(70);
    delayed_cancel_reports.clear();
    delayed_cancel_engine.process_market_trade({32, "DELAY", 100.0, 10, Side::Sell, 0, "test"});
    assert(has_report(delayed_cancel_reports, 70, ExecType::Trade, 10));
    assert(has_report(delayed_cancel_reports, 70, ExecType::Filled, 0));
    assert(std::count_if(delayed_cancel_reports.begin(), delayed_cancel_reports.end(),
                         [](const ExecutionReport& report)
                         { return report.exec_type == ExecType::Cancelled; }) == 0);

    MatchingEngine cancel_engine(nullptr, 0.1, {}, 2);
    std::vector<ExecutionReport> cancel_reports;
    cancel_engine.set_report_callback([&cancel_reports](const ExecutionReport& report)
                                      { cancel_reports.push_back(report); });
    cancel_engine.process_bbo({40, "CANCEL", 100.0, 10, 100.1, 10, 1, "test"});
    cancel_engine.send_order({71, "CANCEL", exchange::Side::Buy,
                              exchange::OrderType::Limit, 100.0, 10, 10, 41,
                              "MarketMaker"});
    cancel_engine.cancel_order(71);
    cancel_engine.cancel_order(71);
    cancel_engine.process_bbo({42, "CANCEL", 100.0, 10, 100.1, 10, 2, "test"});
    assert(std::none_of(cancel_reports.begin(), cancel_reports.end(),
                        [](const ExecutionReport& report)
                        { return report.exec_type == ExecType::Cancelled; }));
    cancel_engine.process_bbo({43, "CANCEL", 100.0, 10, 100.1, 10, 3, "test"});
    assert(std::count_if(cancel_reports.begin(), cancel_reports.end(),
                         [](const ExecutionReport& report)
                         { return report.exec_type == ExecType::Cancelled; }) == 1);
    cancel_engine.cancel_order(71);
    assert(std::count_if(cancel_reports.begin(), cancel_reports.end(),
                         [](const ExecutionReport& report)
                         { return report.exec_type == ExecType::Cancelled; }) == 1);

    const auto trade_report = std::find_if(queued_reports.begin(), queued_reports.end(),
                                           [](const ExecutionReport& report)
                                           { return report.exec_type == ExecType::Trade; });
    assert(trade_report != queued_reports.end());
    assert(trade_report->executed_quantity() == trade_report->quantity);
    assert(trade_report->remaining_quantity() == 0);

    queued_reports.clear();
    queued_engine.process_l2_top({26, "BTCUSDT", 99.0, 30, 100.1, 40, 3, "test"});
    queued_engine.send_order({54, "BTCUSDT", exchange::Side::Buy,
                              exchange::OrderType::Limit, 99.0, 10, 10, 27,
                              "MarketMaker"});
    queued_reports.clear();
    queued_engine.process_market_trade({28, "BTCUSDT", 99.0, 30, Side::Sell, 0, "test"});
    assert(!has_report(queued_reports, 54, ExecType::Trade, 10));

    MatchingEngine fee_engine(
        nullptr, 0.1, FeeSchedule{.maker_rate = 0.001, .taker_rate = 0.002, .quantity_scale = 100});
    std::vector<ExecutionReport> fee_reports;
    fee_engine.set_report_callback([&fee_reports](const ExecutionReport& report)
                                   { fee_reports.push_back(report); });
    fee_engine.send_order({60, "TEST", exchange::Side::Buy, exchange::OrderType::Limit, 10.0, 100,
                           100, 20, "MarketMaker"});
    fee_engine.process_market_trade({21, "TEST", 10.0, 100, Side::Sell, 0, "test"});
    const auto fee_trade =
        std::find_if(fee_reports.begin(), fee_reports.end(), [](const ExecutionReport& report)
                     { return report.exec_type == ExecType::Trade && report.order_id == 60; });
    assert(fee_trade != fee_reports.end());
    assert(fee_trade->liquidity_role == LiquidityRole::Maker);
    assert(std::abs(fee_trade->fee - 0.01) < 1e-12);

    fee_reports.clear();
    fee_engine.process_bbo({30, "TEST", 9.9, 100, 10.0, 100, 2, "test"});
    fee_engine.send_order({61, "TEST", exchange::Side::Buy, exchange::OrderType::Limit, 10.0, 100,
                           100, 31, "MarketMaker"});
    const auto taker_trade =
        std::find_if(fee_reports.begin(), fee_reports.end(), [](const ExecutionReport& report)
                     { return report.exec_type == ExecType::Trade && report.order_id == 61; });
    assert(taker_trade != fee_reports.end());
    assert(taker_trade->liquidity_role == LiquidityRole::Taker);
    assert(std::abs(taker_trade->fee - 0.02) < 1e-12);
}