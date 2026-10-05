#pragma once
#include <map>
#include <mutex>

#include "common/tick.h"
#include "common/types.h"
#include "common/market_event.h"
#include "orderbook/top_of_book.h"

class IOrderBook
{
   public:
    virtual ~IOrderBook() = default;
    virtual void on_bbo(const BboQuote& quote) = 0;
    virtual TopOfBook market_top(const std::string& symbol) = 0;
};

class ILegacyTickBook
{
   public:
    virtual ~ILegacyTickBook() = default;
    virtual void on_tick(const legacy::Tick& t) = 0;
};

class OrderBook : public IOrderBook, public ILegacyTickBook
{
   public:
    explicit OrderBook(double tick_size = PRICE_TICK_SIZE) : tick_size_(tick_size) {}
    void on_tick(const legacy::Tick& t) override;
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