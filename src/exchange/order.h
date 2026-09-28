#pragma once
#include <cstdint>
#include <string>

namespace exchange {

enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };

struct Order {
    uint64_t id;
    std::string symbol;
    Side side;
    OrderType type;
    double price;
    uint64_t qty;
    uint64_t remaining;
    uint64_t ts;
    std::string owner;
};

}  // namespace exchange
