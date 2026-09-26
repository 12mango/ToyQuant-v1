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
    std::string timeline_path;
    if (argc >= 16) timeline_path = argv[15];

    try
    {
        const auto results =
            run_strategy_benchmark(trades_path, quotes_path, symbol, quantity_scale, flow_config,
                                   l1_risk_threshold, l1_stress_spread_multiplier,
                                   l1_minimum_stress_quantity_ratio,
                                   l1_fee_spread_multiplier, timeline_path);
        std::cout << std::left << std::setw(20) << "strategy" << std::setw(12) << "orders"
                  << std::setw(12) << "filled_qty" << std::setw(10) << "fills"
                  << std::setw(12) << "filtered" 
                  << std::setw(10) << "bbo_pct" << std::setw(10) << "avg_gap"
                  << std::setw(10) << "cancel_nf" << std::setw(10) << "avg_life"
                  << std::setw(12) << "position" << std::setw(14) << "gross_pnl"
                  << std::setw(14) << "net_pnl" << std::setw(12) << "fees"
                      << std::setw(12) << "fee_ratio" << std::setw(14) << "edge"
                      << std::setw(14) << "markout" << std::setw(12) << "avg_inv"
                      << std::setw(12) << "max_inv" << std::setw(10) << "inv_flip" << '\n';
        for (const auto& r : results)
        {
            std::cout << std::left << std::setw(20) << r.strategy_name << std::setw(12)
                      << r.submitted_orders << std::setw(12) << r.filled_quantity << std::setw(10)
                      << r.trade_reports << std::setw(12) << r.filtered_market_trades
                        << std::setw(10)
                        << (r.quote_observations == 0
                            ? 0.0
                            : static_cast<double>(r.bbo_quote_observations) /
                                static_cast<double>(r.quote_observations))
                        << std::setw(10)
                        << (r.quote_observations == 0
                            ? 0.0
                            : r.total_quote_distance /
                                static_cast<double>(r.quote_observations))
                        << std::setw(10) << r.cancelled_before_fill_orders
                            << std::setw(10)
                            << (r.quote_observations == 0
                                ? 0.0
                                : static_cast<double>(r.total_order_lifetime_cycles) /
                                    static_cast<double>(r.quote_observations))
                      << std::setw(12) << r.net_position << std::setw(14)
                      << std::fixed << std::setprecision(6) << r.gross_pnl << std::setw(14)
                      << std::fixed << std::setprecision(6) << r.equity - 1000.0 << std::setw(12)
                          << std::fixed << std::setprecision(6) << r.fees_paid << std::setw(12)
                          << r.fee_ratio << std::setw(14) << r.captured_edge << std::setw(14)
                          << r.adverse_selection << std::setw(12) << r.average_abs_inventory
                          << std::setw(12) << r.max_abs_inventory << std::setw(10)
                          << r.inventory_sign_changes << '\n';
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
