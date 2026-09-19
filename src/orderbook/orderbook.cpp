#include "orderbook/orderbook.h"

void OrderBook::on_tick(const legacy::Tick& t)
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto& b = books_[t.symbol];
    const PriceTick price = to_price_tick(t.price, tick_size_);
    if (t.side == Side::Buy)
    {
        b.external_bids_qty[price] = t.size;
    }
    else if (t.side == Side::Sell)
    {
        b.external_asks_qty[price] = t.size;
    }
    else if (t.price > 0)
    {
        b.external_bids_qty[price] = t.size;
    }
}

void OrderBook::on_bbo(const BboQuote& quote)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto& book = books_[quote.symbol];
    book.external_bbo = quote;
    book.has_external_bbo =
        quote.bid_price > 0.0 && quote.ask_price > 0.0 && quote.bid_price <= quote.ask_price;
}

TopOfBook OrderBook::market_top(const std::string& symbol)
{
    std::lock_guard<std::mutex> lock(mtx_);
    TopOfBook out;
    const auto it = books_.find(symbol);
    if (it == books_.end()) return out;

    const auto& book = it->second;
    if (book.has_external_bbo)
    {
        return TopOfBook{book.external_bbo.bid_price, book.external_bbo.bid_quantity,
                         book.external_bbo.ask_price, book.external_bbo.ask_quantity};
    }
    if (!book.external_bids_qty.empty())
    {
        out.bid_price = to_price(book.external_bids_qty.begin()->first, tick_size_);
        out.bid_size = book.external_bids_qty.begin()->second;
    }
    if (!book.external_asks_qty.empty())
    {
        out.ask_price = to_price(book.external_asks_qty.begin()->first, tick_size_);
        out.ask_size = book.external_asks_qty.begin()->second;
    }
    return out;
}
