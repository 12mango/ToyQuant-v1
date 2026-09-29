// The checks in this test are the test: keep them enabled even when the build defines
// NDEBUG, which is the case for the RelWithDebInfo profiling preset.
#ifdef NDEBUG
#undef NDEBUG
#endif

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

    MatchingEngine bbo_engine(0.1);
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

    MatchingEngine queued_engine(0.1);
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

    MatchingEngine conservative_queue_engine(0.1, {}, 0, QueueModel::Conservative);
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

    MatchingEngine optimistic_queue_engine(0.1, {}, 0, QueueModel::Optimistic);
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

    // The proportional model: a level that empties takes the whole queue ahead with it.
    MatchingEngine prorata_queue_engine(0.1, {}, 0, QueueModel::ProRata);
    prorata_queue_engine.process_l2_top({34, "PRORATA", 100.0, 50, 100.1, 40, 2, "test"});
    prorata_queue_engine.send_order({82, "PRORATA", exchange::Side::Buy,
                                     exchange::OrderType::Limit, 100.0, 10, 10, 35,
                                     "MarketMaker"});
    prorata_queue_engine.process_l2_top({36, "PRORATA", 100.0, 0, 100.1, 40, 3, "test"});
    assert(prorata_queue_engine.queue_ahead_from_quantity_changes(Side::Buy) == 50);

    // The measured shape of a level decrease is that whole orders leave, each of them ahead of us with
    // probability q = queue ahead / previous displayed. At q = 1 all of the decrease was ahead of us, so
    // every model except the conservative one must remove exactly the decrease, with no randomness left
    // in it however finely the decrease is split.
    for (const uint64_t chunks : {1ULL, 8ULL})
    {
        MatchingEngine lumpy_full(0.1, {}, 0, QueueModel::Lumpy, chunks, 7);
        lumpy_full.process_l2_top({40, "LUMPYFULL", 100.0, 50, 100.1, 40, 2, "test"});
        lumpy_full.send_order({90, "LUMPYFULL", exchange::Side::Buy, exchange::OrderType::Limit, 100.0,
                               10, 10, 41, "MarketMaker"});
        lumpy_full.process_l2_top({42, "LUMPYFULL", 100.0, 0, 100.1, 40, 3, "test"});
        assert(lumpy_full.queue_ahead_from_quantity_changes(Side::Buy) == 50);
    }

    // q < 1 is what a decrease after the displayed size grew looks like: 50 of a displayed 100 are ahead
    // of us, so the mean removal is half the decrease and the lumpiness is the only thing left to vary.
    const auto half_share_removal = [](uint64_t seed)
    {
        MatchingEngine engine(0.1, {}, 0, QueueModel::Lumpy, 1, seed);
        engine.process_l2_top({50, "LUMPYHALF", 100.0, 50, 100.1, 40, 2, "test"});
        engine.send_order({91, "LUMPYHALF", exchange::Side::Buy, exchange::OrderType::Limit, 100.0, 10,
                           10, 51, "MarketMaker"});
        // New size joins behind us, so FIFO keeps our place: the queue ahead stays 50 while the display
        // reaches 100, and the decrease from 100 back to 50 is then a 50/100 share.
        engine.process_l2_top({52, "LUMPYHALF", 100.0, 100, 100.1, 40, 3, "test"});
        engine.process_l2_top({53, "LUMPYHALF", 100.0, 50, 100.1, 40, 4, "test"});
        return engine.queue_ahead_from_quantity_changes(Side::Buy);
    };
    // The seed makes a run reproducible: one seed is a replication, not a random draw.
    assert(half_share_removal(11) == half_share_removal(11));

    MatchingEngine prorata_half(0.1, {}, 0, QueueModel::ProRata);
    prorata_half.process_l2_top({54, "PRORATAHALF", 100.0, 50, 100.1, 40, 2, "test"});
    prorata_half.send_order({92, "PRORATAHALF", exchange::Side::Buy, exchange::OrderType::Limit, 100.0,
                             10, 10, 55, "MarketMaker"});
    prorata_half.process_l2_top({56, "PRORATAHALF", 100.0, 100, 100.1, 40, 3, "test"});
    prorata_half.process_l2_top({57, "PRORATAHALF", 100.0, 50, 100.1, 40, 4, "test"});
    assert(prorata_half.queue_ahead_from_quantity_changes(Side::Buy) == 25);  // r * q = 50 * 0.5

    // The lumpy model has the same mean by construction, 25, and a variance pro-rata throws away. Over
    // 200 replications the total has to land near 200 * 25, and every single draw has to be a whole
    // order: either the one ahead of us came off, or nothing did.
    uint64_t lumpy_removed_total = 0;
    for (uint64_t seed = 1; seed <= 200; ++seed)
    {
        const uint64_t removed = half_share_removal(seed);
        assert(removed == 0 || removed == 50);
        lumpy_removed_total += removed;
    }
    assert(lumpy_removed_total > 200 * 20 && lumpy_removed_total < 200 * 30);

    // The arrival share scales the queue a new quote starts behind: Q0 = alpha * D0. At 1.0 the engine
    // keeps the FIFO rule untouched, which is what makes every recorded run reproducible.
    MatchingEngine arrival_half(0.1, {}, 0, QueueModel::ProRata, 1, kDefaultQueueSeed, 0.5);
    arrival_half.process_l2_top({60, "ARRIVALHALF", 100.0, 50, 100.1, 40, 2, "test"});
    arrival_half.send_order({93, "ARRIVALHALF", exchange::Side::Buy, exchange::OrderType::Limit, 100.0,
                             10, 10, 61, "MarketMaker"});
    arrival_half.process_l2_top({62, "ARRIVALHALF", 100.0, 10, 100.1, 40, 3, "test"});
    // Half of the display is ahead of us, so a decrease of 40 removes 40 * (25 / 50) = 20 from it.
    assert(arrival_half.queue_ahead_from_quantity_changes(Side::Buy) == 20);

    MatchingEngine arrival_front(0.1, {}, 0, QueueModel::ProRata, 1, kDefaultQueueSeed, 0.0);
    std::vector<ExecutionReport> arrival_front_reports;
    arrival_front.set_report_callback([&arrival_front_reports](const ExecutionReport& report)
                                      { arrival_front_reports.push_back(report); });
    arrival_front.process_l2_top({63, "ARRIVALFRONT", 100.0, 50, 100.1, 40, 2, "test"});
    arrival_front.send_order({94, "ARRIVALFRONT", exchange::Side::Buy, exchange::OrderType::Limit, 100.0,
                              10, 10, 64, "MarketMaker"});
    // Standing at the front of the queue means a print of our size fills us with nothing to wait for.
    arrival_front_reports.clear();
    arrival_front.process_market_trade({65, "ARRIVALFRONT", 100.0, 10, Side::Sell, 0, "test"});
    assert(has_report(arrival_front_reports, 94, ExecType::Trade, 10));

    // Latency decides when an order exists, not how it behaves. With a seven-microsecond latency an order
    // sent at ts=100 lands on the first event whose clock reaches 107, so a print at 100 cannot reach it
    // and a print after the clock has passed 107 can. Note the field order of the market events: the
    // timestamp is the first member, not a sequence number.
    MatchingEngine latency_engine(0.1, {}, 0, QueueModel::ProRata, 1, kDefaultQueueSeed, 0.0, 7);
    std::vector<ExecutionReport> latency_reports;
    latency_engine.set_report_callback([&latency_reports](const ExecutionReport& report)
                                       { latency_reports.push_back(report); });
    latency_engine.process_l2_top({100, "LATENCY", 100.0, 50, 100.1, 40, 70, "test"});
    latency_engine.send_order({95, "LATENCY", exchange::Side::Buy, exchange::OrderType::Limit, 100.0, 10,
                               10, 100, "MarketMaker"});
    assert(!has_report(latency_reports, 95, ExecType::Resting, 10));  // still in flight
    latency_reports.clear();
    latency_engine.process_market_trade({100, "LATENCY", 100.0, 10, Side::Sell, 0, "test"});
    assert(!has_report(latency_reports, 95, ExecType::Trade, 10));  // the exchange does not have it yet
    latency_reports.clear();
    latency_engine.process_l2_top({107, "LATENCY", 100.0, 50, 100.1, 40, 72, "test"});
    assert(has_report(latency_reports, 95, ExecType::Resting, 10));  // the clock passed the delivery
    latency_reports.clear();
    latency_engine.process_market_trade({108, "LATENCY", 100.0, 10, Side::Sell, 0, "test"});
    assert(has_report(latency_reports, 95, ExecType::Trade, 10));

    // A cancel takes effect only after its own latency. The order rests at ts=200, the cancel is sent at
    // 200 with a cancels latency of 50, and the cancellation therefore lands at 250 rather than at the
    // next event: through 240 the quote is still standing and still able to be filled.
    MatchingEngine cancel_latency_engine(0.1, {}, 0, QueueModel::ProRata, 1, kDefaultQueueSeed, 1.0, 0,
                                         50);
    std::vector<ExecutionReport> cancel_latency_reports;
    cancel_latency_engine.set_report_callback([&cancel_latency_reports](const ExecutionReport& report)
                                              { cancel_latency_reports.push_back(report); });
    cancel_latency_engine.process_l2_top({200, "CANCELLAT", 100.0, 50, 100.1, 40, 60, "test"});
    cancel_latency_engine.send_order({96, "CANCELLAT", exchange::Side::Buy, exchange::OrderType::Limit,
                                      100.0, 10, 10, 200, "MarketMaker"});
    assert(has_report(cancel_latency_reports, 96, ExecType::Resting, 10));
    cancel_latency_engine.cancel_order(96);
    cancel_latency_reports.clear();
    cancel_latency_engine.process_l2_top({240, "CANCELLAT", 100.0, 50, 100.1, 40, 61, "test"});
    assert(!has_report(cancel_latency_reports, 96, ExecType::Cancelled, 10));
    cancel_latency_engine.process_l2_top({250, "CANCELLAT", 100.0, 50, 100.1, 40, 62, "test"});
    assert(has_report(cancel_latency_reports, 96, ExecType::Cancelled, 10));

    queued_reports.clear();
    queued_engine.cancel_order(52);
    queued_engine.send_order({53, "BTCUSDT", exchange::Side::Buy,
                              exchange::OrderType::Limit, 100.0, 10, 10, 24,
                              "MarketMaker"});
    queued_reports.clear();
    queued_engine.process_market_trade({25, "BTCUSDT", 100.0, 1, Side::Sell, 0, "test"});
    assert(has_report(queued_reports, 53, ExecType::Trade, 1));
    assert(has_report(queued_reports, 53, ExecType::PartialFill, 9));

    MatchingEngine delayed_cancel_engine(0.1, {}, 1);
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

    MatchingEngine cancel_engine(0.1, {}, 2);
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

    // An order resting behind the touch is not first in the queue either. It used to be given a
    // queue of zero whenever its price was not exactly the best price, so it filled in full as
    // soon as any trade printed through its level.
    {
        MatchingEngine behind_engine(0.1, {});
        std::vector<ExecutionReport> behind_reports;
        behind_engine.set_report_callback([&behind_reports](const ExecutionReport& report)
                                          { behind_reports.push_back(report); });

        // Best bid 99.0 shows 30, so that is the queue in front of a bid one tick lower.
        behind_engine.process_l2_top({900, "TEST", 99.0, 30, 99.1, 30, 1, "test"});
        behind_engine.send_order({91, "TEST", exchange::Side::Buy, exchange::OrderType::Limit,
                                  98.9, 10, 10, 901, "MarketMaker"});
        behind_reports.clear();

        // A sell of 10 prints at 98.9: it only consumes part of the displayed queue.
        behind_engine.process_market_trade({902, "TEST", 98.9, 10, Side::Sell, 0, "test"});
        assert(!has_report(behind_reports, 91, ExecType::Trade, 10));

        // The remaining 20 of the queue plus this order's 10 have to trade before it is filled.
        behind_engine.process_market_trade({903, "TEST", 98.9, 20, Side::Sell, 0, "test"});
        assert(!has_report(behind_reports, 91, ExecType::Trade, 10));

        behind_engine.process_market_trade({904, "TEST", 98.9, 10, Side::Sell, 0, "test"});
        assert(has_report(behind_reports, 91, ExecType::Trade, 10));
    }

    // A public trade consumes the displayed size, so a later taker order cannot fill against
    // liquidity that has already traded. This block used to be gated on the queue model symbol
    // set, which only the L2 entry point populates, so on the L1 path the stored best bid and ask
    // quantities never shrank from public trades.
    {
        MatchingEngine depth_engine(0.1, FeeSchedule{.quantity_scale = 1});
        std::vector<ExecutionReport> depth_reports;
        depth_engine.set_report_callback([&depth_reports](const ExecutionReport& report)
                                         { depth_reports.push_back(report); });

        // Best ask shows 30 at 99.1.
        depth_engine.process_bbo({1000, "TEST", 99.0, 100, 99.1, 30, 1, "test"});
        // The market takes 25 of that level.
        depth_engine.process_market_trade({1001, "TEST", 99.1, 25, Side::Buy, 0, "test"});

        // Only 5 is left to take, so a 20 lot buy fills 5 from the displayed size and rests 15.
        depth_engine.send_order({81, "TEST", exchange::Side::Buy, exchange::OrderType::Limit, 99.1,
                                 20, 20, 1002, "MarketMaker"});
        const auto depth_trade =
            std::find_if(depth_reports.begin(), depth_reports.end(),
                         [](const ExecutionReport& report)
                         {
                             return report.exec_type == ExecType::Trade && report.order_id == 81;
                         });
        assert(depth_trade != depth_reports.end());
        assert(depth_trade->quantity == 5);
        assert(depth_trade->liquidity_role == LiquidityRole::Taker);
        assert(has_report(depth_reports, 81, ExecType::PartialFill, 15));
    }

    MatchingEngine fee_engine(0.1, FeeSchedule{.maker_rate = 0.001, .taker_rate = 0.002, .quantity_scale = 100});
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

    // A contract with a fixed USD face value pays its fee on the face value, not on the base
    // asset price. One Deribit BTC-PERPETUAL contract is worth 10 USD, so a 0.02% maker fee is
    // 0.002 USD. Charging on the BTC price instead would report about 1.27 USD per contract.
    {
        MatchingEngine contract_engine(0.5,
            FeeSchedule{.maker_rate = 0.0002,
                        .taker_rate = 0.0005,
                        .quantity_scale = 1,
                        .unit_notional_usd = 10.0});
        std::vector<ExecutionReport> contract_reports;
        contract_engine.set_report_callback(
            [&contract_reports](const ExecutionReport& report)
            { contract_reports.push_back(report); });

        contract_engine.send_order({71, "BTC-PERPETUAL", exchange::Side::Buy,
                                    exchange::OrderType::Limit, 6374.0, 1, 1, 701,
                                    "MarketMaker"});
        contract_engine.process_market_trade(
            {702, "BTC-PERPETUAL", 6374.0, 1, Side::Sell, 0, "deribit"});

        const auto contract_trade =
            std::find_if(contract_reports.begin(), contract_reports.end(),
                         [](const ExecutionReport& report)
                         {
                             return report.exec_type == ExecType::Trade && report.order_id == 71;
                         });
        assert(contract_trade != contract_reports.end());
        assert(contract_trade->liquidity_role == LiquidityRole::Maker);
        assert(contract_trade->price == 6374.0);
        assert(std::abs(contract_trade->fee - 0.002) < 1e-12);

        // A spot style instrument ignores the field and keeps charging on the price.
        MatchingEngine spot_engine(0.5,
            FeeSchedule{.maker_rate = 0.0002,
                        .taker_rate = 0.0005,
                        .quantity_scale = 1,
                        .unit_notional_usd = 0.0});
        std::vector<ExecutionReport> spot_reports;
        spot_engine.set_report_callback([&spot_reports](const ExecutionReport& report)
                                        { spot_reports.push_back(report); });
        spot_engine.send_order({72, "BTC-PERPETUAL", exchange::Side::Buy,
                                exchange::OrderType::Limit, 6374.0, 1, 1, 801, "MarketMaker"});
        spot_engine.process_market_trade(
            {802, "BTC-PERPETUAL", 6374.0, 1, Side::Sell, 0, "deribit"});
        const auto spot_trade =
            std::find_if(spot_reports.begin(), spot_reports.end(),
                         [](const ExecutionReport& report)
                         {
                             return report.exec_type == ExecType::Trade && report.order_id == 72;
                         });
        assert(spot_trade != spot_reports.end());
        assert(std::abs(spot_trade->fee - 1.2748) < 1e-9);
    }
}