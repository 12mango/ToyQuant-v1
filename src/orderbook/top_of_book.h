#pragma once

#include <cstdint>

// The touch of a book, and the only price information a strategy that does not read depth ever sees.
//
// It lives in a header of its own rather than beside `OrderBook`, because everything that names it --
// `L2MarketView`, every strategy, the pipeline -- is downstream of the L2 book and none of them should
// have to include the legacy book, with its `IOrderBook` interfaces and the `Tick` model those take, in
// order to say what its input is. `orderbook.h` includes this header, so nothing that already reached
// `TopOfBook` through it had to change.
struct TopOfBook
{
    double bid_price{0};
    uint64_t bid_size{0};
    double ask_price{0};
    uint64_t ask_size{0};
};
