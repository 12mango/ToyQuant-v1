#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "exchange/execution_report.h"

class Logger;

// ==================== Backtest Types ====================
struct BacktestExecutionReport
{
    uint64_t ts = 0;
    std::string symbol;
    Side side = Side::Unknown;
    double price = 0.0;
    uint64_t quantity = 0;
    uint64_t order_id = 0;
    ExecType exec_type = ExecType::Trade;
    LiquidityRole liquidity_role = LiquidityRole::Unknown;
    double fee = 0.0;
    bool has_recorded_fee = false;
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
                   const std::string& trades_file, double slippage, double fee_rate, RunMode mode,
                   Logger& logger);
    void run();

   private:
    void print_report();

    std::string tick_file_;
    std::string orders_file_;
    std::string trades_file_;
    double slippage_;
    double fee_rate_;
    RunMode mode_;

    std::unordered_map<std::string, Position> positions;
    std::unordered_map<std::string, double> last_price;
    double realized_pnl = 0.0;
    double total_fees = 0.0;
    double maker_fees = 0.0;
    double taker_fees = 0.0;
    uint64_t maker_trade_count = 0;
    uint64_t taker_trade_count = 0;
    std::vector<double> equity_curve_;

    Logger& logger_;
};
