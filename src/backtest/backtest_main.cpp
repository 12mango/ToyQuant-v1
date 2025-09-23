#include "backtest_driver.h"
#include <string>
#include <iostream>

int main(int argc, char** argv) {
    // 默认路径
    std::string tick_file   = "/home/biaoge/projects/hft-demo/data/sample_ticks.csv";
    std::string orders_file = "/home/biaoge/projects/hft-demo/data/orders.csv";
    std::string trades_file = "/home/biaoge/projects/hft-demo/data/trades.csv";

    if (argc > 1) tick_file   = argv[1];
    if (argc > 2) orders_file = argv[2];
    if (argc > 3) trades_file = argv[3];

    std::cout << "Tick file: " << tick_file << "\n"
              << "Orders file: " << orders_file << "\n"
              << "Trades file: " << trades_file << std::endl;

    BacktestDriver driver(tick_file, orders_file, trades_file);
    driver.run();

    return 0;
}
