#pragma once

#include <cstdint>
#include <string>

struct InstrumentSpec
{
    std::string symbol;
    double tick_size{0.00001};
    uint64_t quantity_scale{1};
    uint64_t lot_size{1};
    uint64_t min_order_quantity{1};
    double maker_fee_rate{0.0};
    double taker_fee_rate{0.0};
};

inline InstrumentSpec btc_usdt_spec(uint64_t quantity_scale = 1000000)
{
    return InstrumentSpec{.symbol = "BTCUSDT",
                          .tick_size = 0.10,
                          .quantity_scale = quantity_scale,
                          .lot_size = 1,
                          .min_order_quantity = 1,
                          .maker_fee_rate = 0.0002,
                          .taker_fee_rate = 0.0004};
}
