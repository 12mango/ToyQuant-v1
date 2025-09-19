#pragma once
#include "../orderbook/orderbook.h"
#include <vector>
#include <string>

struct Order {
    enum Side { BUY, SELL } side;
    std::string symbol;
    double price;
    uint64_t quantity;
};

class Strategy {
public:
    virtual ~Strategy() = default;

    // 收到最新盘口时调用，返回要下的订单
    virtual std::vector<Order> on_top_of_book(const std::string& symbol, const TopOfBook& tob) = 0;
};