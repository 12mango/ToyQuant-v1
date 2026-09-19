#pragma once
#include <cctype>
#include <functional>
#include <list>
#include <map>
#include <string>
#include <unordered_map>

#include "common/types.h"
#include "execution_report.h"
#include "market/market_event.h"
#include "order.h"

class Logger;

struct FeeSchedule
{
    double maker_rate{0.0};
    double taker_rate{0.0};
    uint64_t quantity_scale{1};
};

struct PriceLevel
{
    std::list<exchange::Order> orders;
};

struct MEOrderBook
{
    std::map<PriceTick, PriceLevel, std::greater<PriceTick>> bids;  // Highest price first.
    std::map<PriceTick, PriceLevel> asks;                           // Lowest price first.
};

class IMatchingEngine
{
   public:
    using ReportCallback = std::function<void(const ExecutionReport&)>;
    virtual ~IMatchingEngine() = default;

    virtual void send_order(const exchange::Order& order) = 0;
    virtual void process_bbo(const BboQuote& quote) = 0;
    virtual void process_market_trade(const MarketTrade& trade) = 0;
    virtual void cancel_order(uint64_t order_id) = 0;
    virtual void set_report_callback(ReportCallback cb) = 0;
};

class MatchingEngine : public IMatchingEngine
{
   public:
    using ReportCallback = std::function<void(const ExecutionReport&)>;

    explicit MatchingEngine(Logger* logger = nullptr, double tick_size = PRICE_TICK_SIZE,
                            FeeSchedule fee_schedule = {})
        : logger_(logger), tick_size_(tick_size), fee_schedule_(fee_schedule)
    {
    }

    void send_order(const exchange::Order& order) override;
    void process_bbo(const BboQuote& quote) override;
    void process_market_trade(const MarketTrade& trade) override;
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

    void match_external_bbo(exchange::Order& order);
    void process_market_order(const std::string& symbol, Side side, double price,
                              uint64_t quantity, uint64_t ts);
    void match(MEOrderBook& book, const exchange::Order& incoming, bool rest_incoming);
    void report_trade(const exchange::Order& order, double price, uint64_t quantity,
                      LiquidityRole liquidity_role, uint64_t ts);
    void report(const ExecutionReport& rpt)
    {
        if (report_cb_) report_cb_(rpt);
    }

    std::unordered_map<std::string, MEOrderBook> books_;
    std::unordered_map<std::string, BboQuote> external_bbo_;
    std::unordered_map<uint64_t, exchange::Order*> order_index_;
    ReportCallback report_cb_;
    Logger* logger_;
    double tick_size_;
    FeeSchedule fee_schedule_;
};
