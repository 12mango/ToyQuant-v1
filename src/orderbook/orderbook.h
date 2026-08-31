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

class OrderBook
{
   public:
    void on_tick(const Tick& t);
    TopOfBook top(const std::string& symbol);

   private:
    std::mutex mtx_;
    struct SideBook
    {
        std::map<double, uint64_t, std::greater<double>> bids;
        std::map<double, uint64_t> asks;
    };
    std::map<std::string, SideBook> books_;
};