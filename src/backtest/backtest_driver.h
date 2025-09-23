#pragma once
#include <string>
#include <unordered_map>
#include <iostream>
#include "exchange/execution_report.h"  // 使用你最新的 ExecutionReport
#include "market/market_types.h"        // 使用 Tick

// 持仓信息
struct Position {
    int64_t qty = 0;       // 多头为正，空头为负
    double avg_price = 0;  // 持仓均价
};

enum class Side { Buy, Sell };

class BacktestDriver {
public:
    BacktestDriver(const std::string& tick_file,
                   const std::string& orders_file,
                   const std::string& trades_file);

    void run();

private:
    std::string tick_file_;
    std::string orders_file_;
    std::string trades_file_;

    std::unordered_map<std::string, Position> positions; // 每个合约持仓
    std::unordered_map<std::string, double> last_price;  // 最新价格，用于 unrealized PnL
    double realized_pnl = 0.0;

    void on_trade(const ExecutionReport& trade);
    void print_report();
};
