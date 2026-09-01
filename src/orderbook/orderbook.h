#pragma once
#include <map>
#include <mutex>

#include "common/types.h"

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
    virtual TopOfBook top(const std::string& symbol) = 0;
};

class OrderBook : public IOrderBook
{
   public:
    void on_tick(const Tick& t) override;
    TopOfBook top(const std::string& symbol) override;

   private:
    std::mutex mtx_;
    struct SideBook
    {
        std::map<double, uint64_t, std::greater<double>> bids;
        std::map<double, uint64_t> asks;
    };
    std::map<std::string, SideBook> books_;
};