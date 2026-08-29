#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "backtest/backtest_driver.h"

#ifndef DEFAULT_DATA_DIR
#define DEFAULT_DATA_DIR "data"
#endif

// 辅助函数：解析路径，确保相对路径统一映射到正确的根目录/工作目录下
std::string resolve_path(const std::string& input_path, const std::string& default_dir = "")
{
    namespace fs = std::filesystem;
    fs::path p(input_path);

    // 如果已经是绝对路径，直接返回
    if (p.is_absolute())
    {
        return p.string();
    }

    // 1. 如果直接存在，直接使用
    if (fs::exists(p))
    {
        return p.string();
    }

    // 2. 如果指定了默认目录（如 data/），尝试去默认目录下寻找
    if (!default_dir.empty())
    {
        fs::path fallback = fs::path(default_dir) / p.filename();
        if (fs::exists(fallback))
        {
            return fallback.string();
        }
    }

    return input_path;
}

int main(int argc, char** argv)
{
    std::cout << "[REAL RUNTIME CWD]: " << std::filesystem::current_path() << std::endl;
    std::string default_base = std::string(DEFAULT_DATA_DIR);

    std::string tick_file = default_base + "/synthetic_ticks.csv";
    std::string orders_file = default_base + "/orders.csv";
    std::string trades_file = default_base + "/trades.csv";
    double slippage = 0.0;
    double fee_rate = 0.0;
    RunMode mode = RunMode::Backtest;
    std::string log_file = "logs/backtest.log";

    // 1. 读取 CLI / launch.vs.json 传入的参数
    if (argc > 1) tick_file = argv[1];
    if (argc > 2) orders_file = argv[2];
    if (argc > 3) trades_file = argv[3];
    if (argc > 4) slippage = std::atof(argv[4]);
    if (argc > 5) fee_rate = std::atof(argv[5]);
    if (argc > 6)
    {
        std::string m = argv[6];
        mode = (m == "realtime" ? RunMode::Realtime : RunMode::Backtest);
    }
    if (argc > 7) log_file = argv[7];

    // 2. 智能解析路径
    tick_file = resolve_path(tick_file, default_base);
    orders_file = resolve_path(orders_file, default_base);
    trades_file = resolve_path(trades_file, default_base);
    log_file = resolve_path(log_file);  // 确保 log 路径格式规范

    // 3. 打印当前实际工作目录和路径配置（方便在 VS 输出窗口中排查路径问题）
    std::cout << "[Working Dir]: " << std::filesystem::current_path() << "\n"
              << "Tick file:    " << tick_file << "\n"
              << "Orders file:  " << orders_file << "\n"
              << "Trades file:  " << trades_file << "\n"
              << "Slippage:     " << slippage << "\n"
              << "Fee rate:     " << fee_rate << "\n"
              << "Mode:         " << (mode == RunMode::Realtime ? "Realtime" : "Backtest") << "\n"
              << "Log file:     " << log_file << std::endl;

    // 4. 运行回测驱动
    BacktestDriver driver(tick_file, orders_file, trades_file, slippage, fee_rate, mode, log_file);
    driver.run();

    return 0;
}