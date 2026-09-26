#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "common/types.h"

struct MarketTrade
{
    uint64_t ts{};
    std::string symbol;
    double price{};
    uint64_t quantity{};
    Side aggressor_side{Side::Unknown};
    uint64_t sequence{};
    std::string exchange;
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
    std::string exchange;
};

struct DepthLevel
{
    double price{};
    uint64_t quantity{};
};

struct MarketDepthSnapshot
{
    uint64_t ts{};
    std::string symbol;
    uint64_t sequence{};
    std::vector<DepthLevel> bids;
    std::vector<DepthLevel> asks;
    std::string exchange;
};

struct IncrementalBookUpdate
{
    uint64_t exchange_ts{};
    uint64_t local_ts{};
    std::string symbol;
    bool is_snapshot{false};
    Side side{Side::Unknown};
    double price{};
    uint64_t amount{};
    std::string exchange;
};

struct IncrementalBookBatch
{
    uint64_t ts{};
    uint64_t exchange_ts{};
    uint64_t local_ts{};
    std::string symbol;
    std::vector<IncrementalBookUpdate> updates;
    std::string exchange;
};

using MarketEvent =
    std::variant<MarketTrade, BboQuote, MarketDepthSnapshot, IncrementalBookBatch>;
