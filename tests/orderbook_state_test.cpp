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

    const exchange::Order first{
        1, "EURUSD", exchange::Side::Buy, exchange::OrderType::Limit, 1.10000, 100, 100, 1, "test"};
    const exchange::Order second{
        2, "EURUSD", exchange::Side::Buy, exchange::OrderType::Limit, 1.10000, 50, 50, 2, "test"};

    book.add_order(first);
    book.add_order(second);
    book.add_order(first);
    assert(book.market_top("EURUSD").bid_size == 0);
    assert(book.local_top("EURUSD").bid_size == 150);

    assert(!book.apply_partial_fill(1, 101));
    assert(book.state_for_order(1) == OrderState::Active);
    assert(book.local_top("EURUSD").bid_size == 150);

    assert(book.apply_partial_fill(1, 40));
    assert(book.state_for_order(1) == OrderState::PartialFilled);
    assert(book.local_top("EURUSD").bid_size == 110);

    assert(book.cancel_order(1));
    assert(book.state_for_order(1) == OrderState::Cancelled);
    assert(book.local_top("EURUSD").bid_size == 50);

    assert(book.apply_partial_fill(2, 50));
    assert(book.state_for_order(2) == OrderState::Filled);
    const TopOfBook top = book.local_top("EURUSD");
    assert(top.bid_price == 0.0);
    assert(top.bid_size == 0);
}