#pragma once
#include <cctype>
#include <functional>
#include <list>
#include <map>
#include <string>
#include <unordered_map>

#include "common/types.h"
#include "execution_report.h"
#include "order.h"
#include "orderbook/orderbook.h"

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
    virtual void process_market_tick(const Tick& tick) = 0;
    virtual void cancel_order(uint64_t order_id) = 0;
    virtual void set_report_callback(ReportCallback cb) = 0;
};

class MatchingEngine : public IMatchingEngine
{
   public:
    using ReportCallback = std::function<void(const ExecutionReport&)>;

    void send_order(const exchange::Order& order) override;
    void process_market_tick(const Tick& tick) override;
    void cancel_order(uint64_t order_id) override;

    void set_report_callback(ReportCallback cb) override
    {
        report_cb_ = std::move(cb);
    }

   private:
    static std::string normalize_owner(const std::string& owner)
    {
        std::string normalized;
        normalized.reserve(owner.size());
        for (unsigned char ch : owner)
        {
            if (!std::isspace(ch)) normalized.push_back(static_cast<char>(std::tolower(ch)));
        }
        return normalized;
    }

    bool is_self_trade(const exchange::Order& resting, const exchange::Order& incoming) const
    {
        if (resting.owner.empty() || incoming.owner.empty()) return false;
        return normalize_owner(resting.owner) == normalize_owner(incoming.owner);
    }

    void match(MEOrderBook& book, const exchange::Order& incoming, bool rest_incoming);
    void report(const ExecutionReport& rpt)
    {
        if (report_cb_) report_cb_(rpt);
    }

    std::unordered_map<std::string, MEOrderBook> books_;
    std::unordered_map<uint64_t, exchange::Order*> order_index_;  // 改成 exchange::Order*
    OrderBook private_order_book_;
    ReportCallback report_cb_;
};
