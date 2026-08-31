#pragma once
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>

#include "common/types.h"

// ==================== 回测层专用类型 ====================
enum class RunMode
{
    Backtest,
    Realtime
};
enum class BacktestExecType
{
    Trade
};

struct BacktestExecutionReport
{
    uint64_t ts = 0;
    std::string symbol;
    Side side = Side::Unknown;
    double price = 0.0;
    uint64_t quantity = 0;
    uint64_t order_id = 0;
    BacktestExecType exec_type = BacktestExecType::Trade;
};

struct Position
{
    int64_t qty = 0;
    double avg_price = 0.0;
};

// ==================== BacktestDriver ====================
class BacktestDriver
{
   public:
    BacktestDriver(const std::string& tick_file, const std::string& orders_file,
                   const std::string& trades_file, double slippage = 0.0, double fee_rate = 0.0,
                   RunMode mode = RunMode::Backtest, const std::string& log_file = "");
    ~BacktestDriver();
    void run();

   private:
    void print_report();
    void log(const std::string& msg);

    std::string tick_file_;
    std::string orders_file_;
    std::string trades_file_;
    double slippage_;
    double fee_rate_;
    RunMode mode_;

    std::unordered_map<std::string, Position> positions;
    std::unordered_map<std::string, double> last_price;
    double realized_pnl = 0.0;

    std::ofstream log_file_;
    std::ostream* log_out_;
};
