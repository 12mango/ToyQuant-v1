#pragma once
#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "execution_report.h"
#include "order.h"

struct PriceLevel
{
    std::list<exchange::Order> orders;  // 改成 exchange::Order
};

struct MEOrderBook
{
    std::map<double, PriceLevel, std::greater<double>> bids;  // 买盘（价格从高到低）
    std::map<double, PriceLevel> asks;                        // 卖盘（价格从低到高）
};

class IMatchingEngine
{
   public:
    using ReportCallback = std::function<void(const ExecutionReport&)>;
    virtual ~IMatchingEngine() = default;

    virtual void send_order(const exchange::Order& order) = 0;
    virtual void cancel_order(uint64_t order_id) = 0;
    virtual void set_report_callback(ReportCallback cb) = 0;
};

class MatchingEngine : public IMatchingEngine
{
   public:
    using ReportCallback = std::function<void(const ExecutionReport&)>;

    void send_order(const exchange::Order& order) override;
    void cancel_order(uint64_t order_id) override;

    void set_report_callback(ReportCallback cb) override
    {
        report_cb_ = std::move(cb);
    }

   private:
    void match(MEOrderBook& book, const exchange::Order& incoming);  // 改成 exchange::Order
    void report(const ExecutionReport& rpt)
    {
        if (report_cb_) report_cb_(rpt);
    }

    std::unordered_map<std::string, MEOrderBook> books_;
    std::unordered_map<uint64_t, exchange::Order*> order_index_;  // 改成 exchange::Order*
    ReportCallback report_cb_;
};
