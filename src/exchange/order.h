#pragma once
#include <cstdint>
#include <string>

namespace exchange {

enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };
enum class OrderStatus { New, PartiallyFilled, Filled, Cancelled, Rejected };

struct Order {
    uint64_t id;
    std::string symbol;
    Side side;
    OrderType type;
    double price;
    uint64_t qty;
    uint64_t remaining;
    uint64_t timestamp;
    std::string owner;
};

struct Fill {
    uint64_t order_id;
    double price;
    uint64_t qty;
    uint64_t timestamp;
};

}
