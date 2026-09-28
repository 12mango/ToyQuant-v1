#pragma once

#include <cstdint>
#include <string>

struct InstrumentSpec
{
    std::string exchange;
    std::string symbol;
    double tick_size{0.00001};
    uint64_t quantity_scale{1};
    // USD value of one quantity unit, for instruments where that value does not come from the
    // traded price. 0 means "the traded price is the unit value", which is correct for an
    // instrument quoted in quote currency per unit: one BTCUSDT unit is one BTC and is worth
    // exactly `price` USD.
    //
    // Deribit perpetual contracts are quoted in USD per contract, but one contract has a fixed
    // USD face value of 10. A BTC-PERPETUAL contract is therefore worth 10 USD and not the BTC
    // price, so it sets 10 here. Getting this wrong scales every fee and every cash balance by
    // the price, which is roughly 630x at 2020 BTC prices.
    double unit_notional_usd{0.0};
    uint64_t lot_size{1};
    uint64_t min_order_quantity{1};
    double maker_fee_rate{0.0};
    double taker_fee_rate{0.0};
};

// USD value of one quantity unit at `price`. Shared by the fee model and the portfolio so the
// two cannot disagree about what a filled quantity is worth.
inline double unit_notional_at(double unit_notional_usd, double price)
{
    return unit_notional_usd > 0.0 ? unit_notional_usd : price;
}

// USD value of `quantity` units held from `entry_price` to `price`. With a price based unit this
// is quantity * price. With a fixed USD face value the position holds a fixed USD exposure whose
// base amount was acquired at the entry price, which is the inverse contract relationship, so
// its value scales with the price ratio.
inline double position_value(double unit_notional_usd, double quantity, double entry_price,
                             double price)
{
    if (entry_price <= 0.0) return 0.0;
    return unit_notional_at(unit_notional_usd, entry_price) * quantity * (price / entry_price);
}

inline InstrumentSpec btc_usdt_spec(uint64_t quantity_scale = 1000000)
{
    return InstrumentSpec{.exchange = "binance",
                          .symbol = "BTCUSDT",
                          .tick_size = 0.10,
                          .quantity_scale = quantity_scale,
                          .lot_size = 1,
                          .min_order_quantity = 1,
                          .maker_fee_rate = 0.0002,
                          .taker_fee_rate = 0.0004};
}

inline InstrumentSpec deribit_btc_perpetual_spec()
{
    return InstrumentSpec{.exchange = "deribit",
                          .symbol = "BTC-PERPETUAL",
                          .tick_size = 0.5,
                          .quantity_scale = 1,
                          // One Deribit BTC-PERPETUAL contract is worth a fixed 10 USD.
                          .unit_notional_usd = 10.0,
                          .lot_size = 1,
                          .min_order_quantity = 1,
                          .maker_fee_rate = 0.0002,
                          .taker_fee_rate = 0.0005};
}
