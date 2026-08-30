#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "backtest/backtest_driver.h"

#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif

// 极简且鲁棒的路径转换：如果是相对路径，则强行绑定到项目根目录下；绝对路径保持不变
std::string to_abs_path(const std::string& input_path)
{
    namespace fs = std::filesystem;
    fs::path p(input_path);
    if (p.is_absolute()) {
        return p.string();
    }
    // 强制锚定到 CMake 传入的项目根目录
    return (fs::path(PROJECT_ROOT_DIR) / p).string();
}

int main(int argc, char** argv)
{
    // 默认相对路径
    std::string tick_file   = "data/synthetic_ticks.csv";
    std::string orders_file = "data/orders.csv";
    std::string trades_file = "data/trades.csv";
    std::string log_file    = "logs/backtest.log";
    
    double slippage = 0.0;
    double fee_rate = 0.0;
    RunMode mode    = RunMode::Backtest;

    // 1. 读取 CLI 传入参数
    if (argc > 1) tick_file   = argv[1];
    if (argc > 2) orders_file = argv[2];
    if (argc > 3) trades_file = argv[3];
    if (argc > 4) slippage    = std::atof(argv[4]);
    if (argc > 5) fee_rate    = std::atof(argv[5]);
    if (argc > 6) {
        std::string m = argv[6];
        mode = (m == "realtime" ? RunMode::Realtime : RunMode::Backtest);
    }
    if (argc > 7) log_file    = argv[7];

    // 2. 统一转为根目录下的绝对路径
    tick_file   = to_abs_path(tick_file);
    orders_file = to_abs_path(orders_file);
    trades_file = to_abs_path(trades_file);
    log_file    = to_abs_path(log_file);

    // 自动确保日志的父级目录 (logs) 存在
    std::filesystem::create_directories(std::filesystem::path(log_file).parent_path());

    // 3. 打印启动日志
    std::cout << "[Runtime CWD]: " << std::filesystem::current_path() << "\n"
              << "[Project Root]:" << PROJECT_ROOT_DIR << "\n"
              << "Tick file:     " << tick_file << "\n"
              << "Orders file:   " << orders_file << "\n"
              << "Trades file:   " << trades_file << "\n"
              << "Slippage:      " << slippage << "\n"
              << "Fee rate:      " << fee_rate << "\n"
              << "Mode:          " << (mode == RunMode::Realtime ? "Realtime" : "Backtest") << "\n"
              << "Log file:      " << log_file << std::endl;

    // 4. 运行回测驱动
    BacktestDriver driver(tick_file, orders_file, trades_file, slippage, fee_rate, mode, log_file);
    driver.run();

    return 0;
}