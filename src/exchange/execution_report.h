#pragma once
#include <cstdint>
#include <string>

#include "common/types.h"
#include "order.h"

struct ExecutionReport
{
    uint64_t order_id{};
    exchange::Side side{};
    ExecType exec_type{};
    std::string symbol;
    double price{};
    // Trade reports the fill quantity; all state events report remaining quantity.
    uint64_t quantity{};
    uint64_t ts{};
};
