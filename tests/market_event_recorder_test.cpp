// The check this test makes is the recorder's contract: the events a feed delivered, written back in
// the replay CSV shapes and read again, are the same events in the same order.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "market/l2_replay_feed.h"
#include "market/market_data_adapter.h"
#include "market/market_event_recorder.h"

namespace
{
std::string number(double value)
{
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

// One line per event, carrying everything the file has to reproduce. The readers' own running ids for
// a snapshot and a batch are left out on purpose: the replay formats carry no sequence column for
// them, the readers assign their own, and no decision reads it.
std::string digest(const MarketEvent& event)
{
    return std::visit(
        [](const auto& value)
        {
            using Event = std::decay_t<decltype(value)>;
            std::string out;
            if constexpr (std::is_same_v<Event, MarketTrade>)
            {
                out = "trade ts=" + std::to_string(value.ts) + " " + value.exchange + " " +
                      value.symbol + " px=" + number(value.price) +
                      " qty=" + std::to_string(value.quantity) + " seq=" +
                      std::to_string(value.sequence) + " side=" +
                      (value.aggressor_side == Side::Buy ? "buy" : "sell");
            }
            else if constexpr (std::is_same_v<Event, IncrementalBookBatch>)
            {
                out = "batch ts=" + std::to_string(value.ts) +
                      " exchange_ts=" + std::to_string(value.exchange_ts) +
                      " local_ts=" + std::to_string(value.local_ts) +
                      " snapshot=" + (value.has_snapshot() ? "1" : "0") + " " + value.exchange + " " +
                      value.symbol;
                for (const auto& update : value.updates)
                    out += std::string(" [") + (update.is_snapshot ? "S" : "-") +
                           (update.side == Side::Buy ? "bid" : update.side == Side::Sell ? "ask" : "?") +
                           " " + number(update.price) + "x" + std::to_string(update.amount) + "]";
            }
            else if constexpr (std::is_same_v<Event, MarketDepthSnapshot>)
            {
                out = "snapshot ts=" + std::to_string(value.ts) + " " + value.exchange + " " +
                      value.symbol;
                for (std::size_t level = 0; level < value.bids.size(); ++level)
                    out += std::string(" [b") + number(value.bids[level].price) + "x" +
                           std::to_string(value.bids[level].quantity) + " a" +
                           number(value.asks[level].price) + "x" +
                           std::to_string(value.asks[level].quantity) + "]";
            }
            return out;
        },
        event);
}

struct Run
{
    std::vector<std::string> events;
    MarketDataValidationSummary data;
    RecordingSummary recording;
};

Run replay(const std::string& trades, const std::string& depth, const std::string& record_prefix)
{
    Run run;
    std::unique_ptr<MarketEventRecorder> recorder;
    if (!record_prefix.empty()) recorder = std::make_unique<MarketEventRecorder>(record_prefix);
    L2ReplayFeed feed(make_deribit_trade_reader(trades), make_deribit_depth_reader(depth),
                      [&](const MarketEvent& event)
                      {
                          run.events.push_back(digest(event));
                          if (recorder) recorder->record(event);
                      });
    feed.run();
    if (recorder)
    {
        recorder->flush();
        run.recording = recorder->summary();
    }
    run.data = feed.validation_summary();
    return run;
}

std::size_t line_count(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::string line;
    std::size_t lines = 0;
    while (std::getline(input, line)) ++lines;
    return lines;
}

std::string first_line(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::string line;
    std::getline(input, line);
    return line;
}

}  // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path() / "toy_quant_recorder_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    // An incremental stream, which is the shape the L2 mainline reads: one snapshot batch, then a
    // batch of two level changes that share a local timestamp, then a batch of one.
    const auto trades_path = root / "source_trades.csv";
    const auto depth_path = root / "source_depth.csv";
    {
        std::ofstream trades(trades_path);
        trades << "exchange,symbol,timestamp,local_timestamp,id,side,price,amount\n"
               << "deribit,BTC-PERPETUAL,100,101,10,buy,100.5,2\n"
               << "deribit,BTC-PERPETUAL,300,301,11,sell,100.25,3\n"
               << "deribit,BTC-PERPETUAL,300,302,12,buy,100.75,1\n";
        std::ofstream depth(depth_path);
        depth << "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n"
              << "deribit,BTC-PERPETUAL,50,51,1,bid,100.0,6\n"
              << "deribit,BTC-PERPETUAL,50,51,1,ask,101.0,5\n"
              << "deribit,BTC-PERPETUAL,200,201,0,bid,100.5,8\n"
              << "deribit,BTC-PERPETUAL,200,201,0,ask,101.5,7\n"
              << "deribit,BTC-PERPETUAL,250,251,0,bid,100.75,4\n";
    }

    const Run first = replay(trades_path.string(), depth_path.string(), (root / "recording").string());
    const Run second = replay((root / "recording_trades.csv").string(),
                              (root / "recording_depth.csv").string(), "");
    assert(first.events == second.events);
    // Three trades and three depth batches, one of which is a snapshot.
    assert(first.events.size() == 6);
    assert(second.data.events == 6);
    assert(second.data.trades == 3);
    assert(second.data.incremental_batches == 3);
    assert(second.data.incremental_snapshot_batches == 1);
    // The summary counts rows, so the files have to hold them: a header and one row per trade, and a
    // header and one row per level change.
    assert(second.recording.trades == 0);
    assert(first.recording.trades == 3);
    assert(first.recording.depth_batches == 3);
    assert(first.recording.depth_updates == 5);
    assert(first.recording.depth_snapshots == 0);
    assert(line_count(root / "recording_trades.csv") == 4);
    assert(line_count(root / "recording_depth.csv") == 6);
    assert(first_line(root / "recording_trades.csv") ==
           "exchange,symbol,timestamp,id,side,price,amount");
    assert(first_line(root / "recording_depth.csv") ==
           "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount");

    // A snapshot stream: the depth file is a whole book per row, so the recorded file has to be read
    // by the snapshot reader rather than the incremental one.
    const auto snap_trades = root / "snapshot_trades.csv";
    const auto snap_depth = root / "snapshot_depth.csv";
    {
        std::ofstream trades(snap_trades);
        trades << "exchange,symbol,timestamp,id,side,price,amount\n";
        std::ofstream depth(snap_depth);
        depth << "exchange,symbol,timestamp,asks[0].price,asks[0].amount,bids[0].price,bids[0].amount,"
                 "asks[1].price,asks[1].amount,bids[1].price,bids[1].amount\n"
              << "deribit,BTC-PERPETUAL,10,101.0,5,100.5,6,101.5,7,100.0,8\n"
              << "deribit,BTC-PERPETUAL,20,101.25,4,100.75,3,101.75,2,100.5,1\n";
    }
    const Run snap_first =
        replay(snap_trades.string(), snap_depth.string(), (root / "snaprecording").string());
    const Run snap_second = replay((root / "snaprecording_trades.csv").string(),
                                   (root / "snaprecording_depth.csv").string(), "");
    assert(snap_first.events == snap_second.events);
    assert(snap_second.data.depth_snapshots == 2);
    assert(snap_second.data.incremental_batches == 0);
    assert(snap_first.recording.depth_levels == 2);
    assert(snap_first.recording.depth_snapshots == 2);
    assert(line_count(root / "snaprecording_depth.csv") == 3);

    // What the replay formats cannot express is refused rather than written approximately: a partial
    // file with a hole where the event was reads back as a book that is quietly wrong, which is the
    // failure this class exists to avoid.
    {
        MarketEventRecorder recorder((root / "refused").string());
        bool threw = false;

        BboQuote quote;
        quote.ts = 1;
        try
        {
            recorder.record(quote);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        assert(threw);

        MarketTrade side_less;
        side_less.ts = 1;
        side_less.symbol = "BTC-PERPETUAL";
        side_less.price = 100.0;
        side_less.quantity = 1;
        side_less.aggressor_side = Side::Unknown;
        side_less.sequence = 1;
        side_less.exchange = "deribit";
        threw = false;
        try
        {
            recorder.record(side_less);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        assert(threw);

        MarketTrade comma_symbol = side_less;
        comma_symbol.aggressor_side = Side::Buy;
        comma_symbol.symbol = "BTC,PERPETUAL";
        threw = false;
        try
        {
            recorder.record(comma_symbol);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        assert(threw);
    }
    {
        // A batch is one local timestamp: the reader regroups rows on that field, so a batch whose
        // updates disagree about it cannot be written as the batch it was read as.
        MarketEventRecorder recorder((root / "refused_batch").string());
        IncrementalBookBatch batch;
        batch.ts = 10;
        batch.exchange_ts = 10;
        batch.local_ts = 10;
        batch.symbol = "BTC-PERPETUAL";
        batch.exchange = "deribit";
        batch.snapshot_metadata_valid = true;
        batch.updates.push_back(IncrementalBookUpdate{.exchange_ts = 10,
                                                     .local_ts = 10,
                                                     .is_snapshot = false,
                                                     .side = Side::Buy,
                                                     .price = 100.0,
                                                     .amount = 1});
        batch.updates.push_back(IncrementalBookUpdate{.exchange_ts = 11,
                                                     .local_ts = 11,
                                                     .is_snapshot = false,
                                                     .side = Side::Sell,
                                                     .price = 101.0,
                                                     .amount = 1});
        bool threw = false;
        try
        {
            recorder.record(batch);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        assert(threw);
    }
    {
        // A header is written once, so a snapshot whose level count changes mid-stream has no single
        // file that represents it, and a snapshot whose two sides have different lengths has no
        // header at all.
        MarketDepthSnapshot one;
        one.ts = 1;
        one.symbol = "BTC-PERPETUAL";
        one.exchange = "deribit";
        one.bids.push_back(DepthLevel{100.0, 1});
        one.asks.push_back(DepthLevel{101.0, 1});

        MarketEventRecorder recorder((root / "refused_snapshot").string());
        recorder.record(one);
        MarketDepthSnapshot wider = one;
        wider.bids.push_back(DepthLevel{99.0, 2});
        wider.asks.push_back(DepthLevel{102.0, 2});
        bool threw = false;
        try
        {
            recorder.record(wider);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        assert(threw);

        MarketEventRecorder lopsided((root / "refused_lopsided").string());
        MarketDepthSnapshot unequal = one;
        unequal.asks.push_back(DepthLevel{102.0, 2});
        threw = false;
        try
        {
            lopsided.record(unequal);
        }
        catch (const std::invalid_argument&)
        {
            threw = true;
        }
        assert(threw);
    }

    std::filesystem::remove_all(root);
}
