#include "market/market_data_adapter.h"

#include <cassert>
#include <cmath>
#include <filesystem>
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
    assert(trade.ts == 1711756800000);
    assert(trade.symbol == "BTCUSDT");
    assert(std::abs(trade.price - 69850.53) < 1e-9);
    assert(trade.quantity == 1000);
    assert(trade.aggressor_side == Side::Sell);

    assert(readers.quotes->next(event));
    const auto& quote = std::get<BboQuote>(event);
    assert(quote.ts == 1711756800002);
    assert(std::abs(quote.bid_price - 69903.60) < 1e-9);
    assert(quote.bid_quantity == 462000);
    assert(std::abs(quote.ask_price - 69903.70) < 1e-9);
    assert(quote.ask_quantity == 4419000);

    std::vector<uint64_t> timestamps;
    ReplayFeed feed(
        make_market_data_readers("binance", trades, quotes, instrument),
        [&timestamps](const MarketEvent& value)
        { timestamps.push_back(std::visit([](const auto& item) { return item.ts; }, value)); });
    feed.run();
    assert(!timestamps.empty());
    for (std::size_t index = 1; index < timestamps.size(); ++index)
        assert(timestamps[index - 1] <= timestamps[index]);
}
