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

    book.apply_incremental_batch(IncrementalBookBatch{
        .ts = 300,
        .exchange_ts = 300,
        .local_ts = 301,
        .symbol = "BTC-PERPETUAL",
        .updates = {{.exchange_ts = 300, .local_ts = 301, .symbol = "BTC-PERPETUAL",
                     .is_snapshot = true, .side = Side::Buy, .price = 98.0, .amount = 35},
                    {.exchange_ts = 300, .local_ts = 301, .symbol = "BTC-PERPETUAL",
                     .is_snapshot = true, .side = Side::Sell, .price = 99.0, .amount = 31}},
        .exchange = "deribit"});
    assert(book.top_of_book().bid_price == 98.0);
    assert(book.top_of_book().bid_size == 35);
    assert(book.top_of_book().ask_price == 99.0);
    assert(book.top_of_book().ask_size == 31);
    book.apply_incremental_batch(IncrementalBookBatch{
        .ts = 302,
        .exchange_ts = 302,
        .local_ts = 303,
        .symbol = "BTC-PERPETUAL",
        .updates = {{.exchange_ts = 302, .local_ts = 303, .symbol = "BTC-PERPETUAL",
                     .is_snapshot = true, .side = Side::Buy, .price = 97.0, .amount = 20},
                    {.exchange_ts = 302, .local_ts = 303, .symbol = "BTC-PERPETUAL",
                     .is_snapshot = true, .side = Side::Sell, .price = 100.0, .amount = 20}},
        .exchange = "deribit"});
    assert(book.top_of_book().bid_price == 98.0);
    assert(book.top_of_book().ask_price == 99.0);

    bool rejected_incremental_time = false;
    try
    {
        book.apply_incremental_batch(IncrementalBookBatch{
            .ts = 299,
            .exchange_ts = 299,
            .local_ts = 300,
            .symbol = "BTC-PERPETUAL",
            .updates = {{.exchange_ts = 299,
                         .local_ts = 300,
                         .symbol = "BTC-PERPETUAL",
                         .side = Side::Buy,
                         .price = 98.0,
                         .amount = 1}},
            .exchange = "deribit"});
    }
    catch (const std::invalid_argument&)
    {
        rejected_incremental_time = true;
    }
    assert(rejected_incremental_time);

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