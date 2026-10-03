#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "common/types.h"

// The market's smallest unit of information. It began in the legacy L1 line and is a **core type now**, because the
// order book's own interface takes it: a reader should not have to enter `legacy/` to learn what flows through the
// book. The alias below is the compatibility bridge for the frozen `v1.0-demo` line, which keeps compiling against
// `legacy::Tick` and now means exactly this type.
struct Tick
{
    uint64_t ts = 0;
    std::string symbol;
    double price = 0.0;
    uint64_t size = 0;
    Side side = Side::Unknown;
};

namespace legacy
{
using Tick = ::Tick;
using TickCallback = std::function<void(const Tick&)>;
}  // namespace legacy
