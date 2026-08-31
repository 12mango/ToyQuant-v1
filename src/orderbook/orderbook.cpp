#include "orderbook/orderbook.h"

void OrderBook::on_tick(const Tick& t) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto &b = books_[t.symbol];
    if (t.side == Side::Buy) {
        b.bids[t.price] = b.bids[t.price] + t.size;
    } else if (t.side == Side::Sell) {
        b.asks[t.price] = b.asks[t.price] + t.size;
    } else {
        if (t.price > 0) b.bids[t.price] = b.bids[t.price] + t.size;
    }
}

TopOfBook OrderBook::top(const std::string& symbol) {
    std::lock_guard<std::mutex> lk(mtx_);
    TopOfBook out;
    auto it = books_.find(symbol);
    if(it==books_.end()){
        return out;
    }
    auto &b = it->second;
    if(!b.bids.empty()) {
        out.bid_price = b.bids.begin()->first;
        out.bid_size = b.bids.begin()->second;
    }
    if(!b.asks.empty()) {
        out.ask_price = b.asks.begin()->first;
        out.ask_size = b.asks.begin()->second;
    }
    return out;
}
