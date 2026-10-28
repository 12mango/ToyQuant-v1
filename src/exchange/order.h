#pragma once
#include <cstdint>
#include <string>

#include "common/types.h"

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
    // Kept last so every aggregate initializer written before this field still compiles and gets the
    // good-till-cancelled default that the engine has always implemented.
    TimeInForce time_in_force{TimeInForce::Gtc};
};

}  // namespace exchange
