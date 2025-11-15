#pragma once
#include "../exchange/execution_report.h"
#include "../orderbook/orderbook.h"
#include "strategy.h"
#include <unordered_map>
#include <deque>
#include <numeric>
#include <iostream>
#include <algorithm>
#include <cmath>

class MarketMaker : public Strategy {
public:
    uint64_t base_order_size;
    double base_spread;
    int64_t inventory_limit;
    std::unordered_map<uint64_t, Order> open_orders;
    std::deque<double> mid_prices;
    double last_mid;

    MarketMaker(uint64_t size, double spd, int64_t inv_limit = 1000)
        : base_order_size(size), base_spread(spd), inventory_limit(inv_limit), last_mid(0) {}

    std::vector<Order> on_top_of_book(const std::string& symbol, const TopOfBook& tob) override {
        std::vector<Order> orders;
        static uint64_t next_order_id = 1;

        if (tob.bid_price <= 0 || tob.ask_price <= 0) return orders;

        // 计算当前 mid price 并平滑
        double mid = (tob.bid_price + tob.ask_price) / 2.0;
        mid_prices.push_back(mid);
        if (mid_prices.size() > 10) mid_prices.pop_front();
        double smooth_mid = std::accumulate(mid_prices.begin(), mid_prices.end(), 0.0) / mid_prices.size();

        // 价格趋势
        double trend = smooth_mid - last_mid;
        last_mid = smooth_mid;

        // 动态 spread
        double tick_vol = std::abs(tob.ask_price - tob.bid_price);
        double spread_adj = base_spread;
        if (tick_vol > 0.0002) spread_adj *= 2;

        // 当前仓位计算
        int64_t inventory = 0;
        for (auto& [id, o] : open_orders) {
            inventory += (o.side == Order::BUY ? o.quantity : -int64_t(o.quantity));
        }

        // 动态挂单量
        double buy_ratio = std::max(0.0, 1.0 - double(inventory)/inventory_limit);
        double sell_ratio = std::max(0.0, 1.0 + double(inventory)/inventory_limit);
        uint64_t buy_qty = uint64_t(base_order_size * buy_ratio);
        uint64_t sell_qty = uint64_t(base_order_size * sell_ratio);

        // 买单挂单价格
        if (buy_qty > 0) {
            double buy_price = smooth_mid - spread_adj/2.0 - std::max(0.0, trend);
            Order buy_order(Order::BUY, symbol, buy_price, buy_qty, next_order_id++);
            orders.push_back(buy_order);
            open_orders[buy_order.order_id] = buy_order;
        }

        // 卖单挂单价格
        if (sell_qty > 0) {
            double sell_price = smooth_mid + spread_adj/2.0 + std::max(0.0, trend);
            Order sell_order(Order::SELL, symbol, sell_price, sell_qty, next_order_id++);
            orders.push_back(sell_order);
            open_orders[sell_order.order_id] = sell_order;
        }

        return orders;
    }

    void on_order_update(const ExecutionReport& report) override {
        if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Resting) {
            open_orders.erase(report.order_id);
        } else if (report.exec_type == ExecType::Trade) {
            if (auto it = open_orders.find(report.order_id); it != open_orders.end()) {
                it->second.quantity -= report.quantity;
                if(it->second.quantity == 0) open_orders.erase(it);
            }
        }
    }
};
