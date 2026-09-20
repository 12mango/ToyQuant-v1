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

    if (argc >= 2) trades_path = argv[1];
    if (argc >= 3) quotes_path = argv[2];
    if (argc >= 4) symbol = argv[3];
    if (argc >= 5) quantity_scale = std::stoull(argv[4]);

    try
    {
        const auto results = run_strategy_benchmark(trades_path, quotes_path, symbol, quantity_scale);
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
