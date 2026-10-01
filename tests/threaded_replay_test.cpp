// The threaded path's contract is that it changes when a decision is taken, not what it is: the same
// events handed over a ring have to produce the same run as the direct path, or the numbers a live
// session is compared against are measuring the plumbing.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "market/market_data_adapter.h"
#include "market/threaded_l2_replay.h"

namespace
{
std::string number(double value)
{
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

// One line per delivered event, with the readers' own running ids left out for the same reason the
// recorder leaves them out: no column carries them, so a second read assigns its own.
std::string digest(const MarketEvent& event)
{
    return std::visit(
        [](const auto& value)
        {
            using Event = std::decay_t<decltype(value)>;
            std::string out;
            if constexpr (std::is_same_v<Event, MarketTrade>)
            {
                out = "trade ts=" + std::to_string(value.ts) + " " + value.symbol + " px=" +
                      number(value.price) + " qty=" + std::to_string(value.quantity) + " side=" +
                      (value.aggressor_side == Side::Buy ? "buy" : "sell");
            }
            else if constexpr (std::is_same_v<Event, IncrementalBookBatch>)
            {
                out = "batch ts=" + std::to_string(value.ts) + " " + value.symbol;
                for (const auto& update : value.updates)
                    out += std::string(" [") + (update.side == Side::Buy ? "bid" : "ask") + " " +
                           number(update.price) + "x" + std::to_string(update.amount) + "]";
            }
            else if constexpr (std::is_same_v<Event, MarketDepthSnapshot>)
            {
                out = "snapshot ts=" + std::to_string(value.ts) + " " + value.symbol;
            }
            return out;
        },
        event);
}

void write_inputs(const std::filesystem::path& trades_path, const std::filesystem::path& depth_path,
                  int batches)
{
    std::ofstream trades(trades_path);
    trades << "exchange,symbol,timestamp,local_timestamp,id,side,price,amount\n";
    std::ofstream depth(depth_path);
    depth << "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n";
    for (int index = 0; index < batches; ++index)
    {
        const auto ts = 100 + static_cast<uint64_t>(index) * 10;
        trades << "deribit,BTC-PERPETUAL," << ts + 1 << "," << ts + 1 << "," << index + 1
               << ",buy,100.5,2\n";
        // One snapshot batch to start, then level changes that share a local timestamp, so a batch
        // has to survive the hand-off as a batch.
        depth << "deribit,BTC-PERPETUAL," << ts << "," << ts << (index == 0 ? ",1,bid," : ",0,bid,")
              << number(100.0 + static_cast<double>(index)) << ",6\n";
        depth << "deribit,BTC-PERPETUAL," << ts << "," << ts << (index == 0 ? ",1,ask," : ",0,ask,")
              << number(101.0 + static_cast<double>(index)) << ",5\n";
    }
}
}  // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path() / "toy_quant_threaded_replay_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto trades_path = root / "trades.csv";
    const auto depth_path = root / "depth.csv";
    write_inputs(trades_path, depth_path, 200);

    std::vector<std::string> direct;
    MarketDataValidationSummary direct_data;
    {
        L2ReplayFeed feed(make_deribit_trade_reader(trades_path.string()),
                          make_deribit_depth_reader(depth_path.string()),
                          [&](const MarketEvent& event) { direct.push_back(digest(event)); });
        feed.run();
        direct_data = feed.validation_summary();
    }

    std::vector<std::string> threaded;
    MarketDataValidationSummary threaded_data;
    LatencySummary latency;
    {
        // A ring of eight on purpose: four hundred events through eight slots is mostly backpressure,
        // which is the case a ring that is too small has to survive rather than fail.
        ThreadedReplayFeed feed(make_deribit_trade_reader(trades_path.string()),
                                make_deribit_depth_reader(depth_path.string()),
                                [&](const MarketEvent& event) { threaded.push_back(digest(event)); },
                                {}, {}, 8);
        feed.run();
        threaded_data = feed.validation_summary();
        latency = feed.latency_summary();
    }
    assert(direct == threaded);
    assert(direct.size() == 400);
    assert(threaded_data.events == direct_data.events);
    assert(threaded_data.trades == direct_data.trades);
    assert(threaded_data.incremental_batches == direct_data.incremental_batches);
    assert(threaded_data.incremental_snapshot_batches == direct_data.incremental_snapshot_batches);

    // The latency report describes the same run: one hand-off per event, a ring that never exceeded
    // its capacity by more than the event being processed, and a total that is at least each of the
    // two parts it is made of.
    assert(latency.handoffs == direct.size());
    assert(latency.ring_depth_max >= 1);
    // The depth is what the consumer sees after its own pop, so a ring filled to capacity while the
    // consumer is working reports the capacity plus the event in hand.
    assert(latency.ring_depth_max <= 9);
    assert(latency.total_max_ns >= latency.queue_max_ns);
    assert(latency.total_max_ns >= latency.processing_max_ns);
    assert(latency.total_mean_ns >= latency.queue_mean_ns + latency.processing_mean_ns);
    assert(latency.queue_p99_ns >= latency.queue_p50_ns);
    assert(latency.total_p99_ns >= latency.total_p50_ns);
    // Eight slots make backpressure the normal case here, which is the branch this test exists to
    // cover: a ring that is too small waits rather than dropping.
    assert(latency.feed_waits > 0);
    assert(latency.feed_wait_max_ns >=
           latency.feed_wait_total_ns / static_cast<int64_t>(latency.feed_waits));

    const std::string line = latency.to_log_string();
    assert(line.find("handoffs=400") != std::string::npos);
    assert(line.find("ring_depth_max=") != std::string::npos);
    assert(line.find("feed_waits=") != std::string::npos);

    // A strategy that fails must not leave the feed spinning on a ring that will never be drained:
    // the run has to come back, carrying the strategy's failure rather than a hang or the feed's.
    {
        int calls = 0;
        bool caught = false;
        try
        {
            ThreadedReplayFeed failing(
                make_deribit_trade_reader(trades_path.string()),
                make_deribit_depth_reader(depth_path.string()),
                [&](const MarketEvent&)
                {
                    ++calls;
                    if (calls == 3) throw std::runtime_error("strategy failed");
                },
                {}, {}, 2);
            failing.run();
        }
        catch (const std::runtime_error& error)
        {
            caught = std::string(error.what()) == "strategy failed";
        }
        assert(caught);
    }

    std::filesystem::remove_all(root);
}
