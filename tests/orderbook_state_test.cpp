#include <cassert>

#include "orderbook/orderbook.h"

int main()
{
    OrderBook book;
    const exchange::Order first{
        1, "EURUSD", exchange::Side::Buy, exchange::OrderType::Limit, 1.10000, 100, 100, 1, "test"};
    const exchange::Order second{
        2, "EURUSD", exchange::Side::Buy, exchange::OrderType::Limit, 1.10000, 50, 50, 2, "test"};

    book.add_order(first);
    book.add_order(second);
    assert(book.top("EURUSD").bid_size == 150);

    assert(book.apply_partial_fill(1, 40));
    assert(book.state_for_order(1) == OrderState::PartialFilled);
    assert(book.top("EURUSD").bid_size == 110);

    assert(book.cancel_order(1));
    assert(book.state_for_order(1) == OrderState::Cancelled);
    assert(book.top("EURUSD").bid_size == 50);

    assert(book.apply_partial_fill(2, 50));
    assert(book.state_for_order(2) == OrderState::Filled);
    const TopOfBook top = book.top("EURUSD");
    assert(top.bid_price == 0.0);
    assert(top.bid_size == 0);
}