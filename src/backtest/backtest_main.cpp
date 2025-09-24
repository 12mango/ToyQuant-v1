#include "backtest/backtest_driver.h"
#include <string>
#include <iostream>
#include <cstdlib>

int main(int argc, char** argv) {
    std::string tick_file   = "data/sample_ticks.csv";
    std::string orders_file = "data/orders.csv";
    std::string trades_file = "data/trades.csv";
    double slippage = 0.0;
    double fee_rate = 0.0;

    if(argc > 1) tick_file   = argv[1];
    if(argc > 2) orders_file = argv[2];
    if(argc > 3) trades_file = argv[3];
    if(argc > 4) slippage    = std::atof(argv[4]);
    if(argc > 5) fee_rate    = std::atof(argv[5]);

    std::cout << "Tick file: " << tick_file << "\n"
              << "Orders file: " << orders_file << "\n"
              << "Trades file: " << trades_file << "\n"
              << "Slippage: " << slippage << "\n"
              << "Fee rate: " << fee_rate << std::endl;

    BacktestDriver driver(tick_file, orders_file, trades_file, slippage, fee_rate);
    driver.run();
    return 0;
}
