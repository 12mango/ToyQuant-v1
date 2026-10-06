#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

#include "market/l2_replay_feed.h"
#include "support/test_support.h"

int main()
{
    test_support::TempFile trades_file("l2_replay_trades.csv");
    test_support::TempFile depth_file("l2_replay_depth.csv");
    const auto& trades_path = trades_file.path();
    const auto& depth_path = depth_file.path();
    {
        std::ofstream trades(trades_path);
        trades << "exchange,symbol,timestamp,local_timestamp,id,side,price,amount\n"
               << "deribit,BTC-PERPETUAL,100,101,10,buy,100.5,2\n"
               << "deribit,BTC-PERPETUAL,300,301,11,sell,100.0,3\n";
        std::ofstream depth(depth_path);
        depth << "exchange,symbol,timestamp,asks[0].price,asks[0].amount,bids[0].price,bids[0].amount\n"
              << "deribit,BTC-PERPETUAL,50,101.0,5,100.0,6\n"
              << "deribit,BTC-PERPETUAL,200,101.5,7,100.5,8\n";
    }

    std::vector<uint64_t> timestamps;
    L2ReplayFeed feed(
        make_deribit_trade_reader(trades_path.string()),
        make_deribit_book_snapshot_reader(depth_path.string()),
        [&timestamps](const MarketEvent& event)
        { timestamps.push_back(std::visit([](const auto& value) { return value.ts; }, event)); });
    feed.run();

    assert((timestamps == std::vector<uint64_t>{50, 100, 200, 300}));
    assert(feed.validation_summary().events == 4);
    assert(feed.validation_summary().trades == 2);
    assert(feed.validation_summary().depth_snapshots == 2);
    assert(feed.pacing_summary().events == 0);

    // Pacing delivers each event at its place in a schedule derived from the file's own timestamps, and reports
    // how late it ran. Both the clock and the sleep are injectable, so the schedule is asserted exactly instead
    // of waited for: the two files below interleave into eight events five milliseconds apart.
    test_support::TempFile pace_trades_file("l2_replay_pace_trades.csv");
    test_support::TempFile pace_depth_file("l2_replay_pace_depth.csv");
    const auto& pace_trades = pace_trades_file.path();
    const auto& pace_depth = pace_depth_file.path();
    {
        std::ofstream trades(pace_trades);
        trades << "exchange,symbol,timestamp,local_timestamp,id,side,price,amount\n";
        std::ofstream depth(pace_depth);
        depth << "exchange,symbol,timestamp,asks[0].price,asks[0].amount,bids[0].price,bids[0].amount\n";
        for (int index = 0; index < 4; ++index)
        {
            const auto trade_ts = 1'000'000'000ULL + static_cast<uint64_t>(index) * 10'000'000ULL;
            const auto depth_ts = trade_ts + 5'000'000ULL;
            trades << "deribit,BTC-PERPETUAL," << trade_ts << "," << trade_ts << "," << (10 + index)
                   << ",buy,100.5,2\n";
            depth << "deribit,BTC-PERPETUAL," << depth_ts << ",101.0,5,100.0,6\n";
        }
    }
    {
        int64_t now = 0;
        std::vector<int64_t> waits;
        L2ReplayFeed paced(make_deribit_trade_reader(pace_trades.string()),
                           make_deribit_book_snapshot_reader(pace_depth.string()),
                           [](const MarketEvent&) {}, ReplayPacing{.enabled = true, .speed = 1.0});
        paced.set_clock([&now] { return now; });
        paced.set_sleeper(
            [&](int64_t ns)
            {
                waits.push_back(ns);
                now += ns;
            });
        paced.run();
        assert(waits.size() == 7);
        for (const int64_t wait : waits) assert(wait == 5'000'000);
        assert(paced.pacing_summary().events == 8);
        assert(paced.pacing_summary().late_events == 0);
        assert(paced.pacing_summary().market_window_ns == 35'000'000);
        assert(paced.pacing_summary().wall_ns == 35'000'000);
    }
    {
        // A speed of two delivers the same window in half the wall time, which is how an afternoon checks a day.
        int64_t now = 0;
        std::vector<int64_t> waits;
        L2ReplayFeed fast(make_deribit_trade_reader(pace_trades.string()),
                          make_deribit_book_snapshot_reader(pace_depth.string()),
                          [](const MarketEvent&) {}, ReplayPacing{.enabled = true, .speed = 2.0});
        fast.set_clock([&now] { return now; });
        fast.set_sleeper(
            [&](int64_t ns)
            {
                waits.push_back(ns);
                now += ns;
            });
        fast.run();
        for (const int64_t wait : waits) assert(wait == 2'500'000);
        assert(fast.pacing_summary().wall_ns == 17'500'000);
        assert(fast.pacing_summary().speed == 2.0);
    }
    {
        // A stall longer than the gap is absorbed rather than accumulated, because the schedule is absolute
        // instead of a series of per-event delays: the callback takes twelve milliseconds once, two deliveries
        // are late against it, and the run still finishes on the schedule.
        int64_t now = 0;
        int calls = 0;
        L2ReplayFeed stalled(make_deribit_trade_reader(pace_trades.string()),
                             make_deribit_book_snapshot_reader(pace_depth.string()),
                             [&](const MarketEvent&)
                             {
                                 ++calls;
                                 if (calls == 1) now += 12'000'000;
                             },
                             ReplayPacing{.enabled = true, .speed = 1.0});
        stalled.set_clock([&now] { return now; });
        stalled.set_sleeper([&](int64_t ns) { now += ns; });
        stalled.run();
        const auto& summary = stalled.pacing_summary();
        assert(summary.events == 8);
        assert(summary.late_events == 2);
        assert(summary.lateness_max_ns == 7'000'000);
        assert(summary.lateness_mean_ns == 4'500'000);
        // The percentile is the upper edge of its power-of-two bucket, which is 2048 microseconds for the
        // two millisecond sample that is the smaller of the two.
        assert(summary.lateness_p50_ns == 2'048'000);
        assert(summary.lateness_p99_ns == 2'048'000);
        assert(summary.wall_ns == 35'000'000);
    }
    {
        // One real-time case, so the default sleep is proven wired rather than assumed. The window is 35 ms and
        // the assertion is a floor rather than a measurement: this checks that the feed waits at all.
        const auto started = std::chrono::steady_clock::now();
        L2ReplayFeed real(make_deribit_trade_reader(pace_trades.string()),
                          make_deribit_book_snapshot_reader(pace_depth.string()),
                          [](const MarketEvent&) {}, ReplayPacing{.enabled = true, .speed = 1.0});
        real.run();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        assert(elapsed >= 20);
        assert(real.pacing_summary().events == 8);
    }
}
