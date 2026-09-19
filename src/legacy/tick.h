#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "common/types.h"

namespace legacy
{

struct Tick
{
    uint64_t ts = 0;
    std::string symbol;
    double price = 0.0;
    uint64_t size = 0;
    Side side = Side::Unknown;
};

using TickCallback = std::function<void(const Tick&)>;

}  // namespace legacy
