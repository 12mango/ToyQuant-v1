#include "backtest_driver.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>

// ==================== 安全转换函数 ====================
inline double safe_stod(const std::string& s) {
    std::string str = s;
    str.erase(0, str.find_first_not_of(" \t\r\n"));
    str.erase(str.find_last_not_of(" \t\r\n") + 1);
    if(str.empty()) return 0.0;
    try { return std::stod(str); }
    catch(...) { return 0.0; }
}

inline uint64_t safe_stoull(const std::string& s) {
    std::string str = s;
    str.erase(0, str.find_first_not_of(" \t\r\n"));
    str.erase(str.find_last_not_of(" \t\r\n") + 1);
    if(str.empty()) return 0;
    try { return std::stoull(str); }
    catch(...) { return 0; }
}

// ==================== 构造函数 ====================
BacktestDriver::BacktestDriver(const std::string& tick_file,
                               const std::string& orders_file,
                               const std::string& trades_file)
    : tick_file_(tick_file), orders_file_(orders_file), trades_file_(trades_file)
{}

// ==================== 回测主流程 ====================
void BacktestDriver::run() {
    std::ifstream tick_in(tick_file_);
    std::ifstream trades_in(trades_file_);
    std::string line;

    if(!tick_in.is_open()) {
        std::cerr << "Failed to open tick file: " << tick_file_ << std::endl;
        return;
    }
    if(!trades_in.is_open()) {
        std::cerr << "Failed to open trades file: " << trades_file_ << std::endl;
        return;
    }

    // ==================== 读取 tick 文件 ====================
    std::getline(tick_in, line); // 跳过表头
    while (std::getline(tick_in, line)) {
        if(line.empty()) continue; // 跳过空行
        std::stringstream ss(line);
        Tick t;
        std::string tmp;

        std::getline(ss, tmp, ','); t.ts    = safe_stoull(tmp);
        std::getline(ss, t.symbol, ',');
        std::getline(ss, tmp, ','); t.price = safe_stod(tmp);
        std::getline(ss, tmp, ','); t.size  = safe_stoull(tmp);
        std::getline(ss, tmp, ','); t.side  = tmp.empty() ? ' ' : tmp[0];

        last_price[t.symbol] = t.price; // 更新最新价格
    }

    // ==================== 读取 trades 文件，计算 PnL ====================
    std::getline(trades_in, line); // 跳过表头
    while (std::getline(trades_in, line)) {
        if(line.empty()) continue;
        std::stringstream ss(line);
        ExecutionReport trade;
        std::string tmp, side_str;

        // CSV 列顺: ts,symbol,side,price,quantity,order_id
        //std::getline(ss, tmp, ','); trade.ts = safe_stoull(tmp);       // ts
        std::getline(ss, tmp, ','); // ts，直接丢掉，不存到 ExecutionReport
        std::getline(ss, trade.symbol, ',');                           // symbol
        std::getline(ss, side_str, ',');                                // side
        std::getline(ss, tmp, ','); trade.price    = safe_stod(tmp);    // price
        std::getline(ss, tmp, ','); trade.quantity = safe_stoull(tmp);  // quantity
        std::getline(ss, tmp, ','); trade.order_id = safe_stoull(tmp);  // order_id

        trade.exec_type = ExecType::Trade;

        // ========== PnL 计算 ==========
        auto& pos = positions[trade.symbol];
        int64_t qty = trade.quantity;
        Side s = (side_str == "B" ? Side::Buy : Side::Sell);

        if(s == Side::Buy) {
            // 平空仓
            if(pos.qty < 0) {
                int64_t close_qty = std::min(-pos.qty, qty);
                realized_pnl += close_qty * (pos.avg_price - trade.price);
                pos.qty += close_qty;
                qty -= close_qty;
            }
            // 开多仓
            if(qty > 0) {
                pos.avg_price = (pos.avg_price * pos.qty + trade.price * qty) / (pos.qty + qty);
                pos.qty += qty;
            }
        } else { // Sell
            // 平多仓
            if(pos.qty > 0) {
                int64_t close_qty = std::min(pos.qty, qty);
                realized_pnl += close_qty * (trade.price - pos.avg_price);
                pos.qty -= close_qty;
                qty -= close_qty;
            }
            // 开空仓
            if(qty > 0) {
                pos.avg_price = (pos.avg_price * (-pos.qty) + trade.price * qty) / (-pos.qty + qty);
                pos.qty -= qty;
            }
        }

        // 输出每笔成交实时 PnL
        std::cout << "Trade: " << trade.symbol
                  << " " << (s==Side::Buy?"B":"S")
                  << " " << trade.price
                  << " qty=" << trade.quantity
                  << " RealizedPnL=" << realized_pnl
                  << std::endl;
    }

    // ==================== 输出策略报告 ====================
    print_report();
}

// ==================== 输出策略报告 ====================
void BacktestDriver::print_report() {
    std::cout << "\n=== Strategy Report ===\n";
    std::cout << "Realized PnL: " << realized_pnl << "\n";

    for(const auto& [symbol, pos] : positions) {
        double unrealized_pnl = pos.qty * last_price[symbol];
        std::cout << "Symbol: " << symbol
                  << " Qty: " << pos.qty
                  << " AvgPrice: " << pos.avg_price
                  << " UnrealizedPnL: " << unrealized_pnl
                  << std::endl;
    }
}
