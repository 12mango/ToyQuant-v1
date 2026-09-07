#include "backtest_driver.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "backtest/performance.h"
#include "market/tick_csv_parser.h"
#include "utils/logger.h"

// ==================== Strict Conversion Helpers ====================
inline double safe_stod(const std::string& s)
{
    std::string str = s;
    str.erase(0, str.find_first_not_of(" \t\r\n"));
    str.erase(str.find_last_not_of(" \t\r\n") + 1);
    if (str.empty()) throw std::invalid_argument("empty floating-point value");
    try
    {
        std::size_t parsed = 0;
        const double value = std::stod(str, &parsed);
        if (parsed != str.size()) throw std::invalid_argument("invalid floating-point value");
        return value;
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument("invalid floating-point value: " + str);
    }
}

inline uint64_t safe_stoull(const std::string& s)
{
    std::string str = s;
    str.erase(0, str.find_first_not_of(" \t\r\n"));
    str.erase(str.find_last_not_of(" \t\r\n") + 1);
    if (str.empty()) throw std::invalid_argument("empty integer value");
    try
    {
        std::size_t parsed = 0;
        const uint64_t value = std::stoull(str, &parsed);
        if (parsed != str.size()) throw std::invalid_argument("invalid integer value");
        return value;
    }
    catch (const std::exception&)
    {
        throw std::invalid_argument("invalid integer value: " + str);
    }
}

// ==================== Construction ====================
BacktestDriver::BacktestDriver(const std::string& tick_file, const std::string& orders_file,
                               const std::string& trades_file, double slippage, double fee_rate,
                               RunMode mode, Logger& logger)
    : tick_file_(tick_file),
      orders_file_(orders_file),
      trades_file_(trades_file),
      slippage_(slippage),
      fee_rate_(fee_rate),
      mode_(mode),
      logger_(logger)
{
}

// ==================== Backtest Execution ====================
void BacktestDriver::run()
{
    positions.clear();
    last_price.clear();
    realized_pnl = 0.0;
    equity_curve_.clear();

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

    // ==================== Read Tick Data ====================
    std::getline(tick_in, line);  // Skip the header row.
    std::size_t line_number = 1;
    while (std::getline(tick_in, line))
    {
        ++line_number;
        if (line.empty()) continue;  // Skip empty rows.
        try
        {
            const Tick t = tick_csv::parse_row(line);
            last_price[t.symbol] = t.price;  // Update the latest price.
        }
        catch (const std::exception& ex)
        {
            throw std::invalid_argument("invalid tick row " + std::to_string(line_number) + ": " +
                                        ex.what());
        }
    }

    // ==================== Read Trades and Calculate PnL ====================
    std::getline(trades_in, line);
    constexpr std::string_view source_prefix = "# source_ticks=";
    if (line.rfind(source_prefix, 0) == 0)
    {
        const auto recorded_source = line.substr(source_prefix.size());
        std::error_code ec;
        const auto expected_path = std::filesystem::weakly_canonical(tick_file_, ec);
        const auto recorded_path = std::filesystem::weakly_canonical(recorded_source, ec);
        if (!ec && expected_path != recorded_path)
        {
            throw std::invalid_argument("tick file does not match trades source: expected '" +
                                        expected_path.string() + "', recorded '" +
                                        recorded_path.string() + "'");
        }
        std::getline(trades_in, line);  // Skip the CSV header row.
    }
    line_number = 1;
    while (std::getline(trades_in, line))
    {
        ++line_number;
        if (line.empty()) continue;
        try
        {
            std::stringstream ss(line);
            BacktestExecutionReport trade;
            std::string tmp, side_str;

            // CSV column order: ts,symbol,side,price,quantity,order_id.
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

            if (trade.symbol.empty()) throw std::invalid_argument("empty symbol");
            if (side_str != "B" && side_str != "S")
                throw std::invalid_argument("side must be B or S");
            trade.side = (side_str == "B" ? Side::Buy : Side::Sell);
            trade.exec_type = ExecType::Trade;

            // Apply slippage and fees.
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
                    pos.avg_price =
                        (pos.avg_price * (-pos.qty) + exec_price * qty) / (-pos.qty + qty);
                    pos.qty -= qty;
                }
            }

            logger_.log("Trade: " + trade.symbol + " " + to_char(trade.side) + " " +
                        std::to_string(exec_price) + " qty=" + std::to_string(trade.quantity) +
                        " Fee=" + std::to_string(fee) +
                        " RealizedPnL=" + std::to_string(realized_pnl));
        }
        catch (const std::exception& ex)
        {
            throw std::invalid_argument("invalid trade row " + std::to_string(line_number) + ": " +
                                        ex.what());
        }
    }

    print_report();
}

// ==================== Report Results ====================
void BacktestDriver::print_report()
{
    double equity = realized_pnl;
    std::vector<std::string> symbols;
    symbols.reserve(positions.size());
    for (const auto& [symbol, pos] : positions) symbols.push_back(symbol);
    std::sort(symbols.begin(), symbols.end());

    for (const auto& symbol : symbols)
    {
        const auto& pos = positions.at(symbol);
        double unrealized_pnl = pos.qty * (last_price[symbol] - pos.avg_price);
        equity += unrealized_pnl;
        logger_.log("Symbol: " + symbol + " Qty: " + std::to_string(pos.qty) +
                    " AvgPrice: " + std::to_string(pos.avg_price) +
                    " UnrealizedPnL: " + std::to_string(unrealized_pnl));
    }
    equity_curve_.push_back(equity);

    double max_drawdown = metrics::compute_max_drawdown(equity_curve_);

    logger_.log("\n=== Strategy Report ===");
    logger_.log("Realized PnL: " + std::to_string(realized_pnl));
    logger_.log("Current Equity: " + std::to_string(equity));
    logger_.log("Max Drawdown: " + std::to_string(max_drawdown));
}