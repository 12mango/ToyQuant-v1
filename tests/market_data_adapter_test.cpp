#include "market/market_data_adapter.h"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
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

    MarketDataValidator validator;
    validator.validate(BboQuote{1, "TEST", 100.0, 10, 100.1, 20, 1});
    bool rejected_crossed_quote = false;
    try
    {
        validator.validate(BboQuote{2, "TEST", 100.2, 10, 100.1, 20, 2});
    }
    catch (const std::invalid_argument&)
    {
        rejected_crossed_quote = true;
    }
    assert(rejected_crossed_quote);

    MarketDataValidator sequence_validator;
    sequence_validator.validate(MarketTrade{1, "TEST", 100.0, 1, Side::Buy, 10});
    bool rejected_sequence_regression = false;
    try
    {
        sequence_validator.validate(MarketTrade{2, "TEST", 100.0, 1, Side::Buy, 9});
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
