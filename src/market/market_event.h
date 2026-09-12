#pragma once

#include <cstdint>
#include <string>
#include <variant>

#include "common/types.h"

struct MarketTrade
{
    uint64_t ts{};
    std::string symbol;
    double price{};
    uint64_t quantity{};
    Side aggressor_side{Side::Unknown};
    uint64_t sequence{};
};

struct BboQuote
{
    uint64_t ts{};
    std::string symbol;
    double bid_price{};
    uint64_t bid_quantity{};
    double ask_price{};
    uint64_t ask_quantity{};
    uint64_t sequence{};
};

using MarketEvent = std::variant<MarketTrade, BboQuote>;
