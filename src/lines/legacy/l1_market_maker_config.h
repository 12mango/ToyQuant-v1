// Configuration for the frozen L1 market makers in l1_market_maker.h. Every field here is read by
// those makers; nothing in this header is decorative.
#pragma once

#include <cstddef>
#include <cstdint>

struct L1MarketMakerConfig
{
    uint64_t order_size{50};
    double base_spread{0.00003};
    int64_t inventory_limit{1000};
    double tick_size{0.00001};
    std::size_t trade_imbalance_window{32};
    std::size_t volatility_window{16};
    uint64_t max_quote_age{20};
    double max_book_spread_ratio{0.02};
    double inventory_risk_threshold{0.60};
    double severe_spread_multiplier{20.0};
    double stress_spread_multiplier{2.5};
    double minimum_stress_quantity_ratio{0.10};
    double maker_fee_rate{0.0002};
    double fee_spread_multiplier{0.40};
    uint64_t max_market_trade_age{1000};
    double max_trade_deviation_bps{50.0};
    double flow_trade_weight{0.6};
    double flow_book_weight{0.4};
    double flow_price_threshold{0.06};
    double flow_quantity_threshold{0.08};
    double flow_max_quantity_reduction{0.20};
    uint64_t flow_price_ticks{1};
};

struct FlowAwareMarketMakerConfig
{
    double trade_weight{0.6};
    double book_weight{0.4};
    double price_threshold{0.06};
    double quantity_threshold{0.08};
    double max_quantity_reduction{0.20};
    uint64_t price_ticks{1};
};