#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "app/strategy_benchmark.h"

int main(int argc, char** argv)
{
    std::string trades_path = "data/v2/test_aggTrades_5k.csv";
    std::string quotes_path = "data/v2/test_bookTicker_5k.csv";
    std::string symbol = "BTCUSDT";
    uint64_t quantity_scale = 1000000;
    FlowAwareMarketMakerConfig flow_config;

    if (argc >= 2) trades_path = argv[1];
    if (argc >= 3) quotes_path = argv[2];
    if (argc >= 4) symbol = argv[3];
    if (argc >= 5) quantity_scale = std::stoull(argv[4]);
    if (argc >= 6) flow_config.trade_weight = std::stod(argv[5]);
    if (argc >= 7) flow_config.book_weight = std::stod(argv[6]);
    if (argc >= 8) flow_config.price_threshold = std::stod(argv[7]);
    if (argc >= 9) flow_config.quantity_threshold = std::stod(argv[8]);
    if (argc >= 10) flow_config.max_quantity_reduction = std::stod(argv[9]);
    if (argc >= 11) flow_config.price_ticks = std::stoull(argv[10]);
    double l1_risk_threshold = 0.60;
    double l1_stress_spread_multiplier = 2.5;
    double l1_minimum_stress_quantity_ratio = 0.10;
    double l1_fee_spread_multiplier = 0.40;
    if (argc >= 12) l1_risk_threshold = std::stod(argv[11]);
    if (argc >= 13) l1_stress_spread_multiplier = std::stod(argv[12]);
    if (argc >= 14) l1_minimum_stress_quantity_ratio = std::stod(argv[13]);
    if (argc >= 15) l1_fee_spread_multiplier = std::stod(argv[14]);

    try
    {
        const auto results =
            run_strategy_benchmark(trades_path, quotes_path, symbol, quantity_scale, flow_config,
                                   l1_risk_threshold, l1_stress_spread_multiplier,
                                   l1_minimum_stress_quantity_ratio,
                                   l1_fee_spread_multiplier);
        std::cout << std::left << std::setw(20) << "strategy" << std::setw(12) << "orders"
                  << std::setw(12) << "filled_qty" << std::setw(10) << "fills"
                  << std::setw(12) << "position" << std::setw(14) << "net_pnl"
                  << std::setw(12) << "fees" << '\n';
        for (const auto& r : results)
        {
            std::cout << std::left << std::setw(20) << r.strategy_name << std::setw(12)
                      << r.submitted_orders << std::setw(12) << r.filled_quantity << std::setw(10)
                      << r.trade_reports << std::setw(12) << r.net_position << std::setw(14)
                      << std::fixed << std::setprecision(6) << r.equity - 1000.0 << std::setw(12)
                      << std::fixed << std::setprecision(6)
                      << r.fees_paid << '\n';
        }
        std::cerr << "flow_config trade_weight=" << flow_config.trade_weight
                  << " book_weight=" << flow_config.book_weight
                  << " price_threshold=" << flow_config.price_threshold
                  << " quantity_threshold=" << flow_config.quantity_threshold
                  << " max_quantity_reduction=" << flow_config.max_quantity_reduction
                  << " price_ticks=" << flow_config.price_ticks << '\n';
        if (!results.empty() && results.front().dislocated_trades > 0)
            std::cerr << "benchmark warning: " << results.front().dislocated_trades
                      << " trades exceed 5 bps from the latest BBO; max deviation="
                      << std::fixed << std::setprecision(4)
                      << results.front().max_trade_deviation_bps << " bps\n";
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "benchmark error: " << ex.what() << '\n';
        return 1;
    }
}
