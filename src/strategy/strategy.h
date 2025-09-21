#pragma once
#include "../orderbook/orderbook.h"
#include "../exchange/execution_report.h"
#include <vector>
#include <string>

struct Order {
    enum Side { BUY, SELL } side;
    std::string symbol;
    double price;
    uint64_t quantity;
    uint64_t order_id;

    Order() = default;

    Order(Side s, const std::string& sym, double p, uint64_t qty, uint64_t id)
        : side(s), symbol(sym), price(p), quantity(qty), order_id(id) {}
};

class Strategy {
public:
    virtual ~Strategy() = default;

    // 收到最新盘口时调用，返回要下的订单
    virtual std::vector<Order> on_top_of_book(const std::string& symbol, const TopOfBook& tob) = 0;

    // 收到成交、撤单、挂单等回报时调用
    virtual void on_order_update(const ExecutionReport& rpt) = 0;
};
