// The checks in this test are the test: keep them enabled even when the build defines
// NDEBUG, which is the case for the RelWithDebInfo profiling preset.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "market/market_data_adapter.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "market/replay_feed.h"

#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif

int main()
{
    const auto root = std::filesystem::path(PROJECT_ROOT_DIR);
    const auto trades = (root / "data/v2/test_aggTrades_5k.csv").string();
    const auto quotes = (root / "data/v2/test_bookTicker_5k.csv").string();

    // Two sections below read the repository fixtures under data/v2/, which are not tracked: a fresh clone
    // has none of them, so those sections report that they were skipped instead of failing. Skipping
    // loudly is the point, because a silent skip and a passing check are indistinguishable in a log.
    // line_reader_test uses the same pattern for the same fixture.
    const bool have_l1_fixtures = std::filesystem::exists(trades) && std::filesystem::exists(quotes);

    if (have_l1_fixtures)
    {
        const InstrumentSpec instrument = btc_usdt_spec();
        auto readers = make_market_data_readers("binance", trades, quotes, instrument);
        MarketEvent event;
        assert(readers.trades->next(event));
        const auto& trade = std::get<MarketTrade>(event);
        assert(trade.symbol == "BTCUSDT");
        assert(trade.ts > 0);
        assert(trade.price > 0.0);
        assert(trade.quantity > 0);
        assert(trade.aggressor_side == Side::Buy || trade.aggressor_side == Side::Sell);
        assert(trade.exchange == "binance");

        assert(readers.quotes->next(event));
        const auto& quote = std::get<BboQuote>(event);
        assert(quote.ts > 0);
        assert(quote.bid_price > 0.0);
        assert(quote.ask_price > quote.bid_price);
        assert(quote.bid_quantity > 0);
        assert(quote.ask_quantity > 0);
        assert(quote.exchange == "binance");

        std::vector<uint64_t> timestamps;
        ReplayFeed feed(
            make_market_data_readers("binance", trades, quotes, instrument),
            [&timestamps](const MarketEvent& value)
            { timestamps.push_back(std::visit([](const auto& item) { return item.ts; }, value)); });
        feed.run();
        assert(!timestamps.empty());
        for (std::size_t index = 1; index < timestamps.size(); ++index)
            assert(timestamps[index - 1] <= timestamps[index]);

        const auto& summary = feed.validation_summary();
        assert(summary.events > 0);
        assert(summary.trades > 0);
        assert(summary.quotes > 0);
        assert(summary.events == summary.trades + summary.quotes);
        assert(summary.trades_without_bbo == 0);
        assert(summary.stale_trades <= summary.trades);
        assert(summary.dislocated_trades <= summary.trades);
        std::cout << "market_data_adapter_test: L1 fixture checks ran\n";
    }
    else
    {
        std::cout << "market_data_adapter_test: data/v2 L1 fixtures missing, skipped the replay checks\n";
    }

    MarketDataValidator validator;
    validator.validate(BboQuote{1, "TEST", 100.0, 10, 100.1, 20, 1, "test"});
    bool rejected_crossed_quote = false;
    try
    {
        validator.validate(BboQuote{2, "TEST", 100.2, 10, 100.1, 20, 2, "test"});
    }
    catch (const std::invalid_argument&)
    {
        rejected_crossed_quote = true;
    }
    assert(rejected_crossed_quote);

    MarketDataValidator sequence_validator;
    sequence_validator.validate(MarketTrade{1, "TEST", 100.0, 1, Side::Buy, 10, "test"});
    bool rejected_sequence_regression = false;
    try
    {
        sequence_validator.validate(MarketTrade{2, "TEST", 100.0, 1, Side::Buy, 9, "test"});
    }
    catch (const std::invalid_argument&)
    {
        rejected_sequence_regression = true;
    }
    assert(rejected_sequence_regression);

    const auto snapshot_path = std::filesystem::temp_directory_path() / "toy_quant_depth_snapshot.csv";
    {
        std::ofstream output(snapshot_path);
        output << "exchange,symbol,timestamp,asks[0].price,asks[0].amount,bids[0].price,bids[0].amount,"
                  "asks[1].price,asks[1].amount,bids[1].price,bids[1].amount\n"
              "deribit,BTC-PERPETUAL,100,101.0,2,100.0,3,102.0,4,99.0,5\n"
              "deribit,BTC-PERPETUAL,200,101.5,6,100.5,7,102.5,8,99.5,9\n";
    }

    auto snapshot_reader = make_deribit_book_snapshot_reader(snapshot_path.string());
    MarketEvent snapshot_event;
    assert(snapshot_reader->next(snapshot_event));
    const auto& first_snapshot = std::get<MarketDepthSnapshot>(snapshot_event);
    assert(first_snapshot.symbol == "BTC-PERPETUAL");
    assert(first_snapshot.exchange == "deribit");
    assert(first_snapshot.sequence == 1);
    assert(first_snapshot.bids.size() == 2);
    assert(first_snapshot.asks.size() == 2);
    assert(first_snapshot.bids.front().price == 100.0);
    assert(first_snapshot.asks.front().quantity == 2);
    const auto first_timestamp = first_snapshot.ts;

    assert(snapshot_reader->next(snapshot_event));
    const auto& second_snapshot = std::get<MarketDepthSnapshot>(snapshot_event);
    assert(second_snapshot.sequence == 2);
    assert(second_snapshot.ts > first_timestamp);
    assert(!snapshot_reader->next(snapshot_event));
    std::filesystem::remove(snapshot_path);

    const auto deribit_trades = root / "data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz";
    if (std::filesystem::exists(deribit_trades))
    {
        auto deribit_trade_reader = make_deribit_trade_reader(deribit_trades.string());
        MarketEvent deribit_trade_event;
        assert(deribit_trade_reader->next(deribit_trade_event));
        const auto& deribit_trade = std::get<MarketTrade>(deribit_trade_event);
        assert(deribit_trade.exchange == "deribit");
        assert(deribit_trade.symbol == "BTC-PERPETUAL");
        assert(deribit_trade.ts > 0);
        assert(deribit_trade.sequence == 70745369);
        assert(deribit_trade.price == 6421.5);
        assert(deribit_trade.quantity == 190);
        assert(deribit_trade.aggressor_side == Side::Buy);
    }
    else
    {
        std::cout << "market_data_adapter_test: the Deribit trades fixture is missing, skipped\n";
    }

    // Incremental book reader: batches group rows by local_timestamp, the snapshot batch
    // starts the stream, CRLF rows are accepted, and the final row has no newline.
    const auto incremental_path =
        std::filesystem::temp_directory_path() / "toy_quant_incremental_book.csv";
    {
        std::ofstream output(incremental_path);
        output << "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\r\n"
               << "deribit,BTC-PERPETUAL,400,500,true,bid,6421.5,10\r\n"
               << "deribit,BTC-PERPETUAL,400,500,true,ask,6421.5,10\r\n"
               << "deribit,BTC-PERPETUAL,600,700,false,bid,6421.0,5\r\n"
               << "deribit,BTC-PERPETUAL,600,700,false,bid,6420.5,6\r\n"
               << "deribit,BTC-PERPETUAL,800,900,false,ask,6422.0,8\r\n"
               << "deribit,BTC-PERPETUAL,1000,1100,false,bid,6419.5,3";
    }

    auto incremental_reader = make_deribit_incremental_book_reader(incremental_path.string());
    std::vector<IncrementalBookBatch> batches;
    MarketEvent incremental_event;
    while (incremental_reader->next(incremental_event))
        batches.push_back(std::get<IncrementalBookBatch>(incremental_event));
    assert(!incremental_reader->next(incremental_event));

    assert(batches.size() == 4);
    std::size_t total_updates = 0;
    for (const auto& batch : batches) total_updates += batch.updates.size();
    assert(total_updates == 6);

    assert(batches[0].exchange_ts == 400);
    assert(batches[0].local_ts == 500);
    assert(batches[0].symbol == "BTC-PERPETUAL");
    assert(batches[0].exchange == "deribit");
    assert(batches[0].updates.size() == 2);
    assert(batches[0].has_snapshot());
    assert(batches[0].updates[0].exchange_ts == 400);
    assert(batches[0].updates[0].local_ts == 500);
    assert(batches[0].updates[0].is_snapshot);
    assert(batches[0].updates[0].side == Side::Buy);
    assert(batches[0].updates[0].price == 6421.5);
    assert(batches[0].updates[0].amount == 10);
    assert(batches[0].updates[1].side == Side::Sell);

    assert(batches[1].exchange_ts == 600);
    assert(batches[1].local_ts == 700);
    assert(batches[1].updates.size() == 2);
    assert(!batches[1].has_snapshot());
    assert(!batches[1].updates[0].is_snapshot);
    assert(batches[1].updates[0].price == 6421.0);
    assert(batches[1].updates[0].amount == 5);
    assert(batches[1].updates[1].price == 6420.5);
    assert(batches[1].updates[1].amount == 6);

    assert(batches[2].local_ts == 900);
    assert(batches[2].updates.size() == 1);
    assert(batches[2].updates[0].side == Side::Sell);
    assert(batches[2].updates[0].price == 6422.0);
    assert(batches[2].updates[0].amount == 8);

    assert(batches[3].local_ts == 1100);
    assert(batches[3].updates.size() == 1);
    assert(batches[3].updates[0].side == Side::Buy);
    assert(batches[3].updates[0].price == 6419.5);
    assert(batches[3].updates[0].amount == 3);

    // Header sniffing routes an is_snapshot file to the incremental reader.
    auto routed_reader = make_deribit_depth_reader(incremental_path.string());
    MarketEvent routed_event;
    assert(routed_reader->next(routed_event));
    assert(std::holds_alternative<IncrementalBookBatch>(routed_event));
    std::filesystem::remove(incremental_path);

    // An OKX order book file is JSON Lines: one snapshot line carrying the whole book, then update lines that
    // carry only the levels that changed. Sizes are contract counts, so an amount is the size times the
    // contract value, and a size of zero is how this feed removes a level.
    {
        const auto okx_book = std::filesystem::temp_directory_path() / "okx_book_test.jsonl";
        {
            std::ofstream file(okx_book);
            file << R"({"instId":"BTC-USDT-SWAP","action":"snapshot","ts":"1767744000007",)"
                 << R"("asks":[["93713","248.89","26"],["93713.1","3.88","5"]],)"
                 << R"("bids":[["93712.9","10","1"],["93712.8","0.5","1"]]})" << '\n';
            file << R"({"instId":"BTC-USDT-SWAP","action":"update","ts":"1767744000017",)"
                 << R"("asks":[["93739.1","0.01","1"]],"bids":[]})" << '\n';
            file << R"({"instId":"BTC-USDT-SWAP","action":"update","ts":"1767744000017",)"
                 << R"("asks":[],"bids":[["93750.6","0","0"]]})" << '\n';
            file << R"({"instId":"BTC-USDT-SWAP","action":"update","ts":"1767744000027",)"
                 << R"("asks":[],"bids":[["93708.2","11.9","2"]]})" << '\n';
        }
        auto reader = make_okx_book_reader(okx_book.string(), "BTC-USDT-SWAP", 0.01, 1000000);
        MarketEvent event;
        assert(reader->next(event));
        const auto& snapshot = std::get<MarketDepthSnapshot>(event);
        assert(snapshot.symbol == "BTC-USDT-SWAP");
        assert(snapshot.exchange == "okx");
        assert(snapshot.ts == 1767744000007ULL);
        assert(snapshot.bids.size() == 2);
        assert(snapshot.asks.size() == 2);
        assert(snapshot.asks[0].price == 93713.0);
        // 248.89 contracts of 0.01 BTC is 2.4889 BTC, and the engine counts a millionth of a BTC as one unit.
        assert(snapshot.asks[0].quantity == 2488900);
        assert(snapshot.bids[0].quantity == 100000);

        // Two lines share a timestamp, so they form one batch, and a zero size arrives as a zero amount.
        assert(reader->next(event));
        const auto& first = std::get<IncrementalBookBatch>(event);
        assert(first.exchange == "okx");
        assert(first.ts == 1767744000017ULL);
        assert(first.exchange_ts == first.ts);
        assert(first.local_ts == first.ts);
        assert(!first.has_snapshot());
        assert(first.updates.size() == 2);
        assert(first.updates[0].side == Side::Sell);
        assert(first.updates[0].price == 93739.1);
        assert(first.updates[0].amount == 100);
        assert(first.updates[1].side == Side::Buy);
        assert(first.updates[1].price == 93750.6);
        assert(first.updates[1].amount == 0);

        assert(reader->next(event));
        const auto& second = std::get<IncrementalBookBatch>(event);
        assert(second.ts == 1767744000027ULL);
        assert(second.updates.size() == 1);
        assert(second.updates[0].amount == 119000);
        assert(!reader->next(event));
        std::filesystem::remove(okx_book);
    }

    // The trade reader applies the same contract conversion, and the side in the file is the aggressor's.
    {
        const auto okx_trades = std::filesystem::temp_directory_path() / "okx_trades_test.csv";
        {
            std::ofstream file(okx_trades);
            file << "instrument_name,trade_id,side,price,size,created_time\n";
            file << "BTC-USDT-SWAP,490859504,buy,23517.1,7.0,1677600000026\n";
            file << "ETH-USDT-SWAP,1,sell,1.0,1.0,1677600000026\n";
            file << "BTC-USDT-SWAP,490859505,sell,23517.2,2.5,1677600000100\n";
        }
        auto reader = make_okx_trade_reader(okx_trades.string(), "BTC-USDT-SWAP", 0.01, 1000000);
        MarketEvent event;
        assert(reader->next(event));
        const auto& trade = std::get<MarketTrade>(event);
        assert(trade.symbol == "BTC-USDT-SWAP");
        assert(trade.exchange == "okx");
        assert(trade.sequence == 490859504);
        assert(trade.price == 23517.1);
        // Seven contracts of 0.01 BTC is 0.07 BTC, and the other instrument in the file is skipped.
        assert(trade.quantity == 70000);
        assert(trade.aggressor_side == Side::Buy);
        assert(reader->next(event));
        const auto& later = std::get<MarketTrade>(event);
        assert(later.sequence == 490859505);
        assert(later.quantity == 25000);
        assert(later.aggressor_side == Side::Sell);
        assert(!reader->next(event));
        std::filesystem::remove(okx_trades);
    }

}
