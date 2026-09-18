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

namespace
{
constexpr double InitialCapital = 1000.0;
}  // namespace

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
      portfolio_(1, InitialCapital),
      logger_(logger)
{
}

// ==================== Backtest Execution ====================
void BacktestDriver::run()
{
    portfolio_.reset();
    last_price.clear();
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

    std::vector<Tick> ticks;
    std::vector<BacktestExecutionReport> trades;

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
            ticks.push_back(t);
        }
        catch (const std::exception& ex)
        {
            throw std::invalid_argument("invalid tick row " + std::to_string(line_number) + ": " +
                                        ex.what());
        }
    }

    // ==================== Read Trades and Calculate PnL ====================
    constexpr std::string_view source_prefix = "# source_ticks=";
    while (std::getline(trades_in, line) && line.rfind("# ", 0) == 0)
    {
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
        }
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

            // CSV columns: required legacy fields, followed by optional role and fee.
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

            if (std::getline(ss, tmp, ','))
            {
                if (tmp == "maker")
                    trade.liquidity_role = LiquidityRole::Maker;
                else if (tmp == "taker")
                    trade.liquidity_role = LiquidityRole::Taker;
                else if (tmp != "unknown" && !tmp.empty())
                    throw std::invalid_argument("liquidity role must be maker, taker, or unknown");

                if (std::getline(ss, tmp, ','))
                {
                    trade.fee = safe_stod(tmp);
                    if (trade.fee < 0.0) throw std::invalid_argument("fee cannot be negative");
                    trade.has_recorded_fee = true;
                }
            }

            if (trade.symbol.empty()) throw std::invalid_argument("empty symbol");
            if (side_str != "B" && side_str != "S")
                throw std::invalid_argument("side must be B or S");
            trade.side = (side_str == "B" ? Side::Buy : Side::Sell);
            trade.exec_type = ExecType::Trade;

            trades.push_back(trade);
        }
        catch (const std::exception& ex)
        {
            throw std::invalid_argument("invalid trade row " + std::to_string(line_number) + ": " +
                                        ex.what());
        }
    }

    std::sort(ticks.begin(), ticks.end(),
              [](const Tick& lhs, const Tick& rhs) { return lhs.ts < rhs.ts; });
    std::stable_sort(trades.begin(), trades.end(),
                     [](const BacktestExecutionReport& lhs, const BacktestExecutionReport& rhs)
                     { return lhs.ts < rhs.ts; });

    auto apply_trade = [this](const BacktestExecutionReport& trade)
    {
        double exec_price = trade.price + (trade.side == Side::Buy ? slippage_ : -slippage_);
        double fee = trade.has_recorded_fee ? trade.fee : trade.quantity * exec_price * fee_rate_;
        ExecutionReport report{
            .side = trade.side == Side::Buy ? exchange::Side::Buy : exchange::Side::Sell,
            .exec_type = ExecType::Trade,
            .symbol = trade.symbol,
            .price = exec_price,
            .quantity = trade.quantity,
            .ts = trade.ts,
            .owner = "Backtest",
            .liquidity_role = trade.liquidity_role,
            .fee = fee};
        portfolio_.apply(report);
        const auto portfolio_metrics = portfolio_.metrics();

        logger_.debug("[TRADE] ", trade.symbol, " side=", to_char(trade.side),
                      " price=", exec_price, " qty=", trade.quantity, " fee=", fee,
                      " realized_pnl=", portfolio_metrics.realized_pnl);
    };

    std::size_t trade_index = 0;
    for (const Tick& tick : ticks)
    {
        last_price[tick.symbol] = tick.price;
        while (trade_index < trades.size() && trades[trade_index].ts <= tick.ts)
        {
            apply_trade(trades[trade_index]);
            ++trade_index;
        }
        portfolio_.mark_to_market(last_price);
        equity_curve_.push_back(portfolio_.metrics().equity);
    }
    while (trade_index < trades.size())
    {
        apply_trade(trades[trade_index]);
        ++trade_index;
        portfolio_.mark_to_market(last_price);
        equity_curve_.push_back(portfolio_.metrics().equity);
    }

    print_report();
}

// ==================== Report Results ====================
void BacktestDriver::print_report()
{
    portfolio_.mark_to_market(last_price);
    const auto portfolio_metrics = portfolio_.metrics();
    std::vector<std::string> symbols;
    symbols.reserve(portfolio_.positions().size());
    for (const auto& [symbol, position] : portfolio_.positions()) symbols.push_back(symbol);
    std::sort(symbols.begin(), symbols.end());

    for (const auto& symbol : symbols)
    {
        const auto& position = portfolio_.positions().at(symbol);
        logger_.log("[POSITION] symbol=", symbol, " qty=", position.quantity,
                    " avg_price=", position.average_price);
    }
    if (equity_curve_.empty()) equity_curve_.push_back(portfolio_metrics.equity);
    double max_drawdown = metrics::compute_max_drawdown(equity_curve_);

    logger_.log(
        "[BACKTEST] initial_capital=", InitialCapital,
        " realized_pnl=", portfolio_metrics.realized_pnl,
        " pnl_before_fees=", portfolio_metrics.realized_pnl + portfolio_metrics.fees_paid,
        " unrealized_pnl=", portfolio_metrics.unrealized_pnl, " equity=", portfolio_metrics.equity,
        " fees=", portfolio_metrics.fees_paid, " maker_fees=", portfolio_metrics.maker_fees,
        " taker_fees=", portfolio_metrics.taker_fees,
        " maker_trades=", portfolio_metrics.maker_trade_count,
        " taker_trades=", portfolio_metrics.taker_trade_count, " max_drawdown=", max_drawdown);
}