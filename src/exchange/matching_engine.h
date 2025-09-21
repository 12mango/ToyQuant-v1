#pragma once
#include "execution_report.h"
#include "order.h"
#include <unordered_map>
#include <map>
#include <list>
#include <functional>

struct PriceLevel {
    std::list<exchange::Order> orders;  // 改成 exchange::Order
};

struct MEOrderBook {
    std::map<double, PriceLevel, std::greater<double>> bids; // 买盘（价格从高到低）
    std::map<double, PriceLevel> asks;                       // 卖盘（价格从低到高）
};

class MatchingEngine {
public:
    using ReportCallback = std::function<void(const ExecutionReport&)>;

    void send_order(const exchange::Order& order);
    void cancel_order(uint64_t order_id);

    void set_report_callback(ReportCallback cb) { report_cb_ = std::move(cb); }

private:
    void match(MEOrderBook& book, const exchange::Order& incoming);  // 改成 exchange::Order
    void report(const ExecutionReport& rpt) {
        if (report_cb_) report_cb_(rpt);
    }

    std::unordered_map<std::string, MEOrderBook> books_;
    std::unordered_map<uint64_t, exchange::Order*> order_index_;  // 改成 exchange::Order*
    ReportCallback report_cb_;
};
