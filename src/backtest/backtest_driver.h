#pragma once
#include <string>
#include <unordered_map>
#include <iostream>
#include <fstream>
#include <cstdint>

// ==================== 辅助类型 ====================
enum class Side { Buy, Sell };
enum class ExecType { Trade };
enum class RunMode { Backtest, Realtime };

struct Tick {
    uint64_t ts;
    std::string symbol;
    double price;
    uint64_t size;
    char side;
};

struct ExecutionReport {
    uint64_t ts;
    std::string symbol;
    std::string side;
    double price;
    uint64_t quantity;
    uint64_t order_id;
    ExecType exec_type;
};

struct Position {
    int64_t qty = 0;
    double avg_price = 0.0;
};

// ==================== BacktestDriver ====================
class BacktestDriver {
public:
    BacktestDriver(const std::string& tick_file,
                   const std::string& orders_file,
                   const std::string& trades_file,
                   double slippage = 0.0,
                   double fee_rate = 0.0,
                   RunMode mode = RunMode::Backtest,
                   const std::string& log_file = "");

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
