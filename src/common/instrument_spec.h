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
    // Base asset amount in one quantity unit of the feed, for an instrument quoted in contracts. One OKX
    // BTC-USDT-SWAP contract is 0.01 BTC, so a size of 250 in the file is 2.5 BTC. An instrument whose
    // size is already a base amount leaves this at 1.
    double contract_size{1.0};
    double maker_fee_rate{0.0};
    double taker_fee_rate{0.0};
    // Commission charged for each unit of the instrument, on top of the rates above, in USD. A rate cannot
    // express the cost of a US equity fill: five tenths of a cent a share is 0.5 basis points at a 100 dollar
    // share price and 5 basis points at a 10 dollar one, so the cost of a fill depends on the share price in a
    // way a fraction of notional cannot represent. Every instrument this repository traded before the equity
    // entry leaves it at zero, which is what makes the field additive rather than a change to those schedules.
    double fee_per_share{0.0};
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

// USD value of a fill of `quantity` units at `price`: what a venue charges its rate on, and what a carry's
// funding is measured against. With a price based unit it is `price * quantity`; with a fixed USD face value
// it is the face value times the number of contracts.
inline double fill_notional_usd(double unit_notional_usd, double price, uint64_t quantity,
                                uint64_t quantity_scale)
{
    const double units = quantity_scale > 0
                             ? static_cast<double>(quantity) / static_cast<double>(quantity_scale)
                             : static_cast<double>(quantity);
    return unit_notional_at(unit_notional_usd, price) * units;
}

// USD fee a venue charges for filling `quantity` units at `price`: the USD value of the fill times `rate`,
// plus `fee_per_share` for each unit. The two terms are added rather than chosen between, because a venue can
// charge both and because a per-share commission is what a US equity fill actually costs. Shared by the
// matching engine and by a caller that books a fill outside a book, so the two cannot disagree about what a
// fill costs, exactly as unit_notional_at is shared by the fee model and the portfolio.
inline double fee_on_fill(double rate, double unit_notional_usd, double fee_per_share, double price,
                          uint64_t quantity, uint64_t quantity_scale)
{
    const double units = quantity_scale > 0
                             ? static_cast<double>(quantity) / static_cast<double>(quantity_scale)
                             : static_cast<double>(quantity);
    return fill_notional_usd(unit_notional_usd, price, quantity, quantity_scale) * rate +
           units * fee_per_share;
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

// A linear swap, so the unit value is the traded price times the base amount the contract carries, which is
// what the zero in unit_notional_usd means. The derived order size that strategy_factory computes from
// quantity_scale is a thousandth of a base unit here and therefore below the venue's one contract minimum,
// so a run on this instrument sets --order-size and --inventory-limit explicitly.
inline InstrumentSpec okx_btc_swap_spec(uint64_t quantity_scale = 1000000)
{
    return InstrumentSpec{.exchange = "okx",
                          .symbol = "BTC-USDT-SWAP",
                          .tick_size = 0.1,
                          .quantity_scale = quantity_scale,
                          .lot_size = 1,
                          .min_order_quantity = 10000,
                          // One contract is 0.01 BTC, so with this scale the minimum order is 10000 units.
                          .contract_size = 0.01,
                          .maker_fee_rate = 0.0002,
                          .taker_fee_rate = 0.0005};
}

// A US equity or ETF as IEX publishes it: prices in whole cents, sizes in shares. This is the first instrument
// in this repository where the fee is not a multiple of a tick — one tick on a hundred dollar share is one
// basis point, and a retail commission of a few tenths of a cent a share is a fraction of it, where the
// perpetuals this repository traded before paid tens of ticks or more. The commission is per share and the rate
// is zero, because that is how a US broker prices an equity fill; the default is the tiered rate of a large
// retail broker, and a zero-commission plan is `fee_per_share = 0.0`.
inline InstrumentSpec us_equity_spec(const std::string& symbol, double fee_per_share = 0.0035)
{
    return InstrumentSpec{.exchange = "iex",
                          .symbol = symbol,
                          .tick_size = 0.01,
                          // IEX publishes sizes in shares, so one quantity unit is one share and its USD value
                          // is the traded price, the spot case.
                          .quantity_scale = 1,
                          .unit_notional_usd = 0.0,
                          .lot_size = 1,
                          .min_order_quantity = 1,
                          .maker_fee_rate = 0.0,
                          .taker_fee_rate = 0.0,
                          .fee_per_share = fee_per_share};
}

// The specification this repository carries for a symbol, looked up by the name the feeds, the instruments
// and the funding files all use. False means nothing is registered for it, and a caller says what it was
// looking for rather than getting a default that would price the wrong instrument.
inline bool instrument_spec_of(const std::string& symbol, InstrumentSpec& out)
{
    if (symbol == "BTCUSDT")
    {
        out = btc_usdt_spec();
        return true;
    }
    if (symbol == "BTC-PERPETUAL")
    {
        out = deribit_btc_perpetual_spec();
        return true;
    }
    if (symbol == "BTC-USDT-SWAP")
    {
        out = okx_btc_swap_spec();
        return true;
    }
    if (symbol == "SPY")
    {
        out = us_equity_spec(symbol);
        return true;
    }
    return false;
}
