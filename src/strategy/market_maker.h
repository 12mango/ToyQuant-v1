#pragma once
#include "../exchange/execution_report.h"
#include "../orderbook/orderbook.h"
#include "strategy.h"
#include <unordered_map>
#include <iostream>

class MarketMaker : public Strategy {
public:
    uint64_t order_size;
    double spread;
    std::unordered_map<uint64_t, Order> open_orders;

    MarketMaker(uint64_t size, double spd) : order_size(size), spread(spd) {}

    std::vector<Order> on_top_of_book(const std::string& symbol, const TopOfBook& tob) override {
        std::vector<Order> orders;
        static uint64_t next_order_id = 1;

        if (tob.bid_price > 0) {
            Order buy_order(Order::BUY, symbol, tob.bid_price - spread, order_size, next_order_id++);
            orders.push_back(buy_order);
            open_orders[buy_order.order_id] = buy_order;
        }

        if (tob.ask_price > 0) {
            Order sell_order(Order::SELL, symbol, tob.ask_price + spread, order_size, next_order_id++);
            orders.push_back(sell_order);
            open_orders[sell_order.order_id] = sell_order;
        }

        return orders;
    }

    inline const char* exec_type_str(ExecType t) {
    switch(t) {
        case ExecType::Trade: return "Trade";
        case ExecType::Cancelled: return "Cancelled";
        case ExecType::Resting: return "Resting";
        default: return "Unknown";
    }
}

void on_order_update(const ExecutionReport& report) override {
    std::cout << "MarketMaker received report: order_id=" << report.order_id
              << " exec_type=" << exec_type_str(report.exec_type)
              << " filled_qty=" << report.quantity
              << " price=" << report.price << "\n";

    if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Resting) {
        open_orders.erase(report.order_id);
    } else if (report.exec_type == ExecType::Trade) {
        if (auto it = open_orders.find(report.order_id); it != open_orders.end()) {
            it->second.quantity -= report.quantity;
        }
    }
}


};
