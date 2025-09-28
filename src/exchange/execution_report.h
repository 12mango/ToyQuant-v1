#pragma once
#include <string>
#include <cstdint>
#include "order.h"

enum class ExecType {
    Trade,
    Cancelled,
    Resting
};

struct ExecutionReport {
    uint64_t order_id{};
    exchange::Side side{};
    ExecType exec_type{};
    std::string symbol;
    double price{};
    uint64_t quantity{};
    uint64_t ts{};
};
