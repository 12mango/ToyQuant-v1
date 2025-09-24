#pragma once
#include <string>
#include <cstdint>

enum class ExecType {
    Trade,
    Cancelled,
    Resting
};

struct ExecutionReport {
    uint64_t order_id{};
    ExecType exec_type{};
    std::string symbol;
    double price{};
    uint64_t quantity{};
    uint64_t ts{};
};
