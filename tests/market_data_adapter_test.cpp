#include "market/market_data_adapter.h"

#include <cassert>
#include <cmath>
#include <filesystem>
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

    assert(readers.quotes->next(event));
    const auto& quote = std::get<BboQuote>(event);
    assert(quote.ts > 0);
    assert(quote.bid_price > 0.0);
    assert(quote.ask_price > quote.bid_price);
    assert(quote.bid_quantity > 0);
    assert(quote.ask_quantity > 0);

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
}
