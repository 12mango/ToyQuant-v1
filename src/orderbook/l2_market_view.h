#pragma once

#include <cstdint>
#include <string>

#include "orderbook/orderbook.h"

struct L2MarketView
{
    std::string symbol;
    uint64_t ts{0};
    TopOfBook top;
    double micro_price{0.0};
    double depth_imbalance{0.0};
    uint64_t bid_depth{0};
    uint64_t ask_depth{0};
};
