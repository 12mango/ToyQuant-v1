#pragma once
#include <map>
#include <mutex>

#include "common/types.h"
#include "market/market_event.h"

struct TopOfBook
{
    double bid_price{0};
    uint64_t bid_size{0};
    double ask_price{0};
    uint64_t ask_size{0};
};

class IOrderBook
{
   public:
    virtual ~IOrderBook() = default;
    virtual void on_tick(const Tick& t) = 0;
    virtual void on_bbo(const BboQuote& quote) = 0;
    virtual TopOfBook market_top(const std::string& symbol) = 0;
};

class OrderBook : public IOrderBook
{
   public:
    explicit OrderBook(double tick_size = PRICE_TICK_SIZE) : tick_size_(tick_size) {}
    void on_tick(const Tick& t) override;
    void on_bbo(const BboQuote& quote) override;
    TopOfBook market_top(const std::string& symbol) override;

   private:
    mutable std::mutex mtx_;
    struct SideBook
    {
        std::map<PriceTick, uint64_t, std::greater<PriceTick>> external_bids_qty;
        std::map<PriceTick, uint64_t> external_asks_qty;
        BboQuote external_bbo;
        bool has_external_bbo{false};
    };
    double tick_size_;
    std::map<std::string, SideBook> books_;
};