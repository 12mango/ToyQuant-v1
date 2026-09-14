#include <cassert>
#include <cmath>

#include "orderbook/orderbook.h"

int main()
{
    OrderBook book;
    book.on_bbo({1, "BTCUSDT", 69900.10, 12, 69900.20, 34, 100});
    auto bbo = book.market_top("BTCUSDT");
    assert(std::abs(bbo.bid_price - 69900.10) < 1e-9);
    assert(bbo.bid_size == 12);
    assert(std::abs(bbo.ask_price - 69900.20) < 1e-9);
    assert(bbo.ask_size == 34);

    book.on_bbo({2, "BTCUSDT", 69899.90, 20, 69900.00, 25, 101});
    bbo = book.market_top("BTCUSDT");
    assert(std::abs(bbo.bid_price - 69899.90) < 1e-9);
    assert(std::abs(bbo.ask_price - 69900.00) < 1e-9);

    book.on_tick({3, "EURUSD", 1.10000, 150, Side::Buy});
    const TopOfBook top = book.market_top("EURUSD");
    assert(std::abs(top.bid_price - 1.10000) < 1e-9);
    assert(top.bid_size == 150);
}