#pragma once
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"

// ==================== 回测层专用类型 ====================
struct BacktestExecutionReport
{
    uint64_t ts = 0;
    std::string symbol;
    Side side = Side::Unknown;
    double price = 0.0;
    uint64_t quantity = 0;
    uint64_t order_id = 0;
    ExecType exec_type = ExecType::Trade;
};

struct Position
{
    int64_t qty = 0;
    double avg_price = 0.0;
};

namespace metrics
{
inline double compute_fill_rate(uint64_t submitted, uint64_t filled)
{
    if (submitted == 0) return 0.0;
    return static_cast<double>(filled) / static_cast<double>(submitted);
}

inline double compute_cancel_rate(uint64_t submitted, uint64_t cancelled)
{
    if (submitted == 0) return 0.0;
    return static_cast<double>(cancelled) / static_cast<double>(submitted);
}

inline double compute_max_drawdown(const std::vector<double>& equity)
{
    if (equity.empty()) return 0.0;

    double peak = equity.front();
    double max_drawdown = 0.0;
    for (double value : equity)
    {
        if (value > peak) peak = value;
        double drawdown = (peak > 0.0) ? (peak - value) / peak : 0.0;
        if (drawdown > max_drawdown) max_drawdown = drawdown;
    }
    return max_drawdown;
}

inline double compute_inventory_exposure(int64_t position)
{
    return static_cast<double>(std::llabs(position));
}
}  // namespace metrics

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
    std::vector<double> equity_curve_;

    std::ofstream log_file_;
    std::ostream* log_out_;
};
