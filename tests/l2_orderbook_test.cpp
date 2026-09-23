#include <cassert>
#include <cmath>
#include <stdexcept>

#include "orderbook/l2_orderbook.h"

int main()
{
    L2OrderBook book;
    book.apply_snapshot(MarketDepthSnapshot{
        .ts = 100,
        .symbol = "BTC-PERPETUAL",
        .sequence = 1,
        .bids = {{100.0, 10}, {99.0, 20}},
        .asks = {{101.0, 11}, {102.0, 21}},
    });

    const auto top = book.top_of_book();
    assert(std::abs(top.bid_price - 100.0) < 1e-9);
    assert(top.bid_size == 10);
    assert(std::abs(top.ask_price - 101.0) < 1e-9);
    assert(top.ask_size == 11);
    assert(book.depth(Side::Buy) == 2);
    assert(book.depth(Side::Sell) == 2);
    assert(book.level(Side::Buy, 1).price == 99.0);
    const auto view = book.market_view();
    assert(view.bid_depth == 30);
    assert(view.ask_depth == 32);
    assert(view.depth_imbalance < 0.0);
    assert(view.micro_price > view.top.bid_price);

    book.apply_snapshot(MarketDepthSnapshot{
        .ts = 200,
        .symbol = "BTC-PERPETUAL",
        .sequence = 2,
        .bids = {{98.0, 30}},
        .asks = {{99.0, 31}},
    });
    assert(book.depth(Side::Buy) == 1);
    assert(book.top_of_book().bid_size == 30);

    bool rejected_sequence = false;
    try
    {
        book.apply_snapshot(MarketDepthSnapshot{200, "BTC-PERPETUAL", 2, {{98.0, 30}}, {{99.0, 31}}});
    }
    catch (const std::invalid_argument&)
    {
        rejected_sequence = true;
    }
    assert(rejected_sequence);
}