#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>

enum class Side : uint8_t
{
    Unknown = 0,
    Buy,
    Sell
};

enum class RunMode : uint8_t
{
    Backtest = 0,
    Realtime
};

enum class ExecType : uint8_t
{
    Trade = 0,
    PartialFill,
    Filled,
    Cancelled,
    Resting
};

using PriceTick = int64_t;

inline constexpr double PRICE_TICK_SIZE = 0.00001;

inline PriceTick to_price_tick(double price, double tick_size = PRICE_TICK_SIZE)
{
    return static_cast<PriceTick>(std::llround(price / tick_size));
}

inline double to_price(PriceTick tick, double tick_size = PRICE_TICK_SIZE)
{
    return static_cast<double>(tick) * tick_size;
}

struct Tick
{
    uint64_t ts = 0;
    std::string symbol;
    double price = 0.0;
    uint64_t size = 0;
    Side side = Side::Unknown;
};

using TickCallback = std::function<void(const Tick&)>;

inline Side to_side(char c)
{
    switch (std::toupper(static_cast<unsigned char>(c)))
    {
        case 'B':
            return Side::Buy;
        case 'S':
            return Side::Sell;
        default:
            return Side::Unknown;
    }
}

inline char to_char(Side s)
{
    switch (s)
    {
        case Side::Buy:
            return 'B';
        case Side::Sell:
            return 'S';
        default:
            return 'N';
    }
}