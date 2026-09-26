#pragma once
#include <cstdint>
#include <string>

#include "common/types.h"
#include "order.h"

enum class LiquidityRole : uint8_t
{
    Unknown = 0,
    Maker,
    Taker
};

struct ExecutionReport
{
    uint64_t order_id{};
    exchange::Side side{};
    ExecType exec_type{};
    std::string symbol;
    double price{};
    // Trade reports this execution's fill quantity. PartialFill, Filled, Resting, and
    // Cancelled report the order's remaining quantity.
    uint64_t quantity{};
    uint64_t ts{};
    std::string owner;
    LiquidityRole liquidity_role{LiquidityRole::Unknown};
    double fee{0.0};

    uint64_t executed_quantity() const
    {
        return exec_type == ExecType::Trade ? quantity : 0;
    }

    uint64_t remaining_quantity() const
    {
        return exec_type == ExecType::Trade ? 0 : quantity;
    }
};
