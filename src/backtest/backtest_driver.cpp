#include "backtest_driver.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

// ==================== 安全转换函数 ====================
inline double safe_stod(const std::string& s)
{
    std::string str = s;
    str.erase(0, str.find_first_not_of(" \t\r\n"));
    str.erase(str.find_last_not_of(" \t\r\n") + 1);
    if (str.empty()) return 0.0;
    try
    {
        return std::stod(str);
    }
    catch (...)
    {
        return 0.0;
    }
}

inline uint64_t safe_stoull(const std::string& s)
{
    std::string str = s;
    str.erase(0, str.find_first_not_of(" \t\r\n"));
    str.erase(str.find_last_not_of(" \t\r\n") + 1);
    if (str.empty()) return 0;
    try
    {
        return std::stoull(str);
    }
    catch (...)
    {
        return 0;
    }
}

// ==================== 构造函数 ====================
BacktestDriver::BacktestDriver(const std::string& tick_file, const std::string& orders_file,
                               const std::string& trades_file, double slippage, double fee_rate,
                               RunMode mode, const std::string& log_file)
    : tick_file_(tick_file),
      orders_file_(orders_file),
      trades_file_(trades_file),
      slippage_(slippage),
      fee_rate_(fee_rate),
      mode_(mode),
      log_out_(&std::cout)
{
    if (!log_file.empty())
    {
        std::filesystem::path p(log_file);

        if (p.has_parent_path() && !p.parent_path().empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
            if (ec)
            {
                std::cerr << "[Error] Failed to create log directory: " << ec.message()
                          << std::endl;
            }
        }

        errno = 0;  // 重置 errno
        log_file_.open(log_file);

        if (log_file_.is_open())
        {
            log_out_ = &log_file_;
            std::cout << "[SUCCESS] Log file opened: " << log_file << std::endl;
        }
        else
        {
            // 打印具体的系统错误原因（比如 Permission denied）
            std::cerr << "[ERROR] Failed to open log file: " << log_file
                      << " | Reason: " << std::strerror(errno)
                      << " | CWD: " << std::filesystem::current_path() << std::endl;
        }
    }
}

// ==================== 析构函数 (确保缓冲区落盘) ====================
BacktestDriver::~BacktestDriver()
{
    if (log_file_.is_open())
    {
        log_file_.flush();
        log_file_.close();
    }
}

// ==================== 日志函数 ====================
void BacktestDriver::log(const std::string& msg)
{
    (*log_out_) << msg << std::endl;
}

// ==================== 回测主流程 ====================
void BacktestDriver::run()
{
    std::ifstream tick_in(tick_file_);
    std::ifstream trades_in(trades_file_);
    std::string line;

    if (!tick_in.is_open())
    {
        std::cerr << "Failed to open tick file: " << tick_file_ << std::endl;
        return;
    }
    if (!trades_in.is_open())
    {
        std::cerr << "Failed to open trades file: " << trades_file_ << std::endl;
        return;
    }

    // ==================== 读取 tick 文件 ====================
    std::getline(tick_in, line);  // 跳过表头
    while (std::getline(tick_in, line))
    {
        if (line.empty()) continue;  // 跳过空行
        std::stringstream ss(line);
        Tick t;
        std::string tmp;

        std::getline(ss, tmp, ',');
        t.ts = safe_stoull(tmp);
        std::getline(ss, t.symbol, ',');
        std::getline(ss, tmp, ',');
        t.price = safe_stod(tmp);
        std::getline(ss, tmp, ',');
        t.size = safe_stoull(tmp);
        std::getline(ss, tmp, ',');
        t.side = tmp.empty() ? Side::Unknown : to_side(tmp[0]);

        last_price[t.symbol] = t.price;  // 更新最新价格
    }

    // ==================== 读取 trades 文件，计算 PnL ====================
    std::getline(trades_in, line);  // 跳过表头
    while (std::getline(trades_in, line))
    {
        if (line.empty()) continue;
        std::stringstream ss(line);
        BacktestExecutionReport trade;
        std::string tmp, side_str;

        // CSV 列顺: ts,symbol,side,price,quantity,order_id
        std::getline(ss, tmp, ',');
        trade.ts = safe_stoull(tmp);          // ts
        std::getline(ss, trade.symbol, ',');  // symbol
        std::getline(ss, side_str, ',');      // side
        std::getline(ss, tmp, ',');
        trade.price = safe_stod(tmp);  // price
        std::getline(ss, tmp, ',');
        trade.quantity = safe_stoull(tmp);  // quantity
        std::getline(ss, tmp, ',');
        trade.order_id = safe_stoull(tmp);  // order_id

        trade.side = (side_str == "B" ? Side::Buy : Side::Sell);
        trade.exec_type = ExecType::Trade;

        // 应用滑点和手续费
        double exec_price = trade.price + (trade.side == Side::Buy ? slippage_ : -slippage_);
        double fee = trade.quantity * exec_price * fee_rate_;

        auto& pos = positions[trade.symbol];
        int64_t qty = static_cast<int64_t>(trade.quantity);
        Side s = trade.side;

        if (s == Side::Buy)
        {
            if (pos.qty < 0)
            {
                int64_t close_qty = std::min(-pos.qty, qty);
                realized_pnl += close_qty * (pos.avg_price - exec_price) - fee;
                pos.qty += close_qty;
                qty -= close_qty;
            }
            if (qty > 0)
            {
                pos.avg_price = (pos.avg_price * pos.qty + exec_price * qty) / (pos.qty + qty);
                pos.qty += qty;
            }
        }
        else
        {  // Sell
            if (pos.qty > 0)
            {
                int64_t close_qty = std::min(pos.qty, qty);
                realized_pnl += close_qty * (exec_price - pos.avg_price) - fee;
                pos.qty -= close_qty;
                qty -= close_qty;
            }
            if (qty > 0)
            {
                pos.avg_price = (pos.avg_price * (-pos.qty) + exec_price * qty) / (-pos.qty + qty);
                pos.qty -= qty;
            }
        }

        log("Trade: " + trade.symbol + " " + to_char(trade.side) + " " +
            std::to_string(exec_price) + " qty=" + std::to_string(trade.quantity) +
            " Fee=" + std::to_string(fee) + " RealizedPnL=" + std::to_string(realized_pnl));
    }

    print_report();
}

// ==================== 输出策略报告 ====================
void BacktestDriver::print_report()
{
    log("\n=== Strategy Report ===");
    log("Realized PnL: " + std::to_string(realized_pnl));

    for (const auto& [symbol, pos] : positions)
    {
        double unrealized_pnl = pos.qty * (last_price[symbol] - pos.avg_price);
        log("Symbol: " + symbol + " Qty: " + std::to_string(pos.qty) + " AvgPrice: " +
            std::to_string(pos.avg_price) + " UnrealizedPnL: " + std::to_string(unrealized_pnl));
    }
}