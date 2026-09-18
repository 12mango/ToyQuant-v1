#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

#include "accounting/portfolio.h"
#include "app/pipeline.h"
#include "app/strategy_factory.h"
#include "backtest/backtest_driver.h"
#include "backtest/performance.h"
#include "common/instrument_spec.h"
#include "exchange/matching_engine.h"
#include "market/csv_feed.h"
#include "market/replay_feed.h"
#include "market/udp_feed.h"
#include "orderbook/orderbook.h"
#include "utils/logger.h"

#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif

namespace
{
struct AppConfig
{
    std::string mode = "csv";
    std::string path_or_port = "data/scenarios/synthetic_ticks.csv";
    std::string quotes_path;
    std::string symbol;
    int delay = 0;
    std::string strategy_name = "optimized";
    uint64_t quantity_scale = 1000000;
};

std::string to_abs_path(const std::string& input_path)
{
    namespace fs = std::filesystem;
    fs::path p(input_path);
    if (p.is_absolute()) return p.string();
    return (fs::path(PROJECT_ROOT_DIR) / p).string();
}

bool parse_integer(const std::string& value, int& result)
{
    if (value.empty()) return false;

    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool parse_unsigned(const std::string& value, uint64_t& result)
{
    if (value.empty()) return false;

    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

void print_usage(const char* executable)
{
    std::cerr << "Usage: " << executable << " csv [path_to_csv] [ms_delay] [strategy]\n";
    std::cerr << "   or: " << executable << " udp <port> [strategy]\n";
    std::cerr << "   or: " << executable
              << " replay <agg_trades_csv> <bbo_csv> <symbol> [ms_delay] [strategy] "
                 "[quantity_scale]\n";
    std::cerr << "   strategy: optimized (default) | naive | l1\n";
}

bool parse_config(int argc, char** argv, AppConfig& cfg, std::string& error)
{
    if (argc == 2 && std::string(argv[1]) == "--help") return false;

    if (argc >= 2) cfg.mode = argv[1];
    if (cfg.mode != "csv" && cfg.mode != "udp" && cfg.mode != "replay")
    {
        error = "mode must be 'csv', 'udp', or 'replay'";
        return false;
    }

    if (cfg.mode == "replay")
    {
        if (argc < 5 || argc > 8)
        {
            error = "replay requires trade file, BBO file, and symbol";
            return false;
        }
        cfg.path_or_port = argv[2];
        cfg.quotes_path = argv[3];
        cfg.symbol = argv[4];
        if (!std::filesystem::is_regular_file(to_abs_path(cfg.path_or_port)) ||
            !std::filesystem::is_regular_file(to_abs_path(cfg.quotes_path)))
        {
            error = "replay input file does not exist";
            return false;
        }
        if (argc >= 6 && (!parse_integer(argv[5], cfg.delay) || cfg.delay < 0))
        {
            error = "delay must be a non-negative integer";
            return false;
        }
        if (argc >= 7) cfg.strategy_name = argv[6];
        if (argc >= 8 && (!parse_unsigned(argv[7], cfg.quantity_scale) || cfg.quantity_scale == 0))
        {
            error = "quantity scale must be a positive integer";
            return false;
        }
    }
    else if (argc > 5 || (cfg.mode == "udp" && argc > 4))
    {
        error = "too many arguments";
        return false;
    }

    if (cfg.mode != "replay" && argc >= 3) cfg.path_or_port = argv[2];
    if (cfg.mode == "csv" && cfg.path_or_port.empty())
    {
        error = "CSV path cannot be empty";
        return false;
    }
    if (cfg.mode == "csv" && !std::filesystem::is_regular_file(to_abs_path(cfg.path_or_port)))
    {
        error = "CSV file does not exist: " + to_abs_path(cfg.path_or_port);
        return false;
    }

    if (cfg.mode != "replay" && argc >= 4)
    {
        if (cfg.mode == "udp")
        {
            cfg.strategy_name = argv[3];
        }
        else if (!parse_integer(argv[3], cfg.delay) || cfg.delay < 0)
        {
            error = "delay must be a non-negative integer";
            return false;
        }
    }
    if (cfg.mode != "replay" && argc >= 5) cfg.strategy_name = argv[4];

    if (cfg.strategy_name != "optimized" && cfg.strategy_name != "naive" &&
        cfg.strategy_name != "l1")
    {
        error = "strategy must be 'optimized', 'naive', or 'l1'";
        return false;
    }

    if (cfg.mode == "udp")
    {
        int port = 0;
        if (!parse_integer(cfg.path_or_port, port) || port < 1 || port > 65535)
        {
            error = "UDP port must be an integer from 1 to 65535";
            return false;
        }
    }

    return true;
}

struct OutputFiles
{
    std::ofstream orders;
    std::ofstream trades;
};

OutputFiles open_output_files(const std::string& source,
                              const std::string& source_type = "source_ticks")
{
    const std::string orders_file = to_abs_path("data/runtime/orders.csv");
    const std::string trades_file = to_abs_path("data/runtime/trades.csv");
    const auto output_dir = std::filesystem::path(orders_file).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec)
    {
        throw std::runtime_error("failed to create output directory '" + output_dir.string() +
                                 "': " + ec.message());
    }

    OutputFiles files{std::ofstream(orders_file), std::ofstream(trades_file)};
    if (!files.orders.is_open() || !files.trades.is_open())
    {
        throw std::runtime_error("failed to open runtime output files in '" + output_dir.string() +
                                 "'");
    }
    files.orders << "# " << source_type << "=" << source << "\n";
    files.trades << "# " << source_type << "=" << source << "\n";
    files.orders << "ts,symbol,side,price,quantity,order_id\n";
    files.trades << "ts,symbol,side,price,quantity,order_id,liquidity_role,fee\n";
    return files;
}

void run_csv_mode(const AppConfig& cfg)
{
    std::string csv_file = to_abs_path(cfg.path_or_port);
    Logger logger(to_abs_path("logs/toy_quant.log"));
    logger.log("[Mode: CSV] Opening: ", csv_file, " (delay: ", cfg.delay,
               "ms) strategy=", cfg.strategy_name);

    auto output_files = open_output_files(csv_file);
    OrderBook order_book;
    auto strategy = make_strategy(cfg.strategy_name);
    MatchingEngine engine(&logger);
    Portfolio portfolio;
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine,
                      portfolio, logger);
    CsvFeed feed(
        csv_file, [&](const Tick& tick) { pipeline.process_tick(tick, true); }, cfg.delay, &logger);
    feed.run();
    logger.log(pipeline.summary().to_log_string());
}

void run_replay_mode(const AppConfig& cfg)
{
    if (cfg.symbol != "BTCUSDT")
        throw std::invalid_argument("no instrument specification for replay symbol: " + cfg.symbol);
    const InstrumentSpec instrument = btc_usdt_spec(cfg.quantity_scale);
    const std::string trades_file = to_abs_path(cfg.path_or_port);
    const std::string quotes_file = to_abs_path(cfg.quotes_path);
    Logger logger(to_abs_path("logs/toy_quant.log"));
    logger.log("[Mode: Replay] trades=", trades_file, " quotes=", quotes_file,
               " symbol=", cfg.symbol, " quantity_scale=", cfg.quantity_scale,
               " strategy=", cfg.strategy_name);

    const std::string source = "binance;trades=" + trades_file + ";quotes=" + quotes_file +
                               ";symbol=" + cfg.symbol +
                               ";tick_size=" + std::to_string(instrument.tick_size) +
                               ";quantity_scale=" + std::to_string(instrument.quantity_scale) +
                               ";maker_fee=" + std::to_string(instrument.maker_fee_rate) +
                               ";taker_fee=" + std::to_string(instrument.taker_fee_rate);
    auto output_files = open_output_files(source, "source_market_data");
    OrderBook order_book(instrument.tick_size);
    auto strategy = make_strategy(cfg.strategy_name, &instrument);
    MatchingEngine engine(&logger, instrument.tick_size,
                          FeeSchedule{.maker_rate = instrument.maker_fee_rate,
                                      .taker_rate = instrument.taker_fee_rate,
                                      .quantity_scale = instrument.quantity_scale});
    Portfolio portfolio(instrument.quantity_scale);
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine,
                      portfolio, logger);
    ReplayFeed feed(
        make_market_data_readers("binance", trades_file, quotes_file, instrument),
        [&](const MarketEvent& event) { pipeline.process_event(event); }, cfg.delay);
    feed.run();
    const auto& validation = feed.validation_summary();
    logger.log("[DATA] events=", validation.events, " trades=", validation.trades,
               " quotes=", validation.quotes, " trades_without_bbo=", validation.trades_without_bbo,
               " stale_trades=", validation.stale_trades,
               " dislocated_trades=", validation.dislocated_trades,
               " max_bbo_age_ms=", validation.max_bbo_age_ms,
               " max_trade_deviation_bps=", validation.max_trade_deviation_bps);
    logger.log(pipeline.summary().to_log_string());
}

void run_udp_mode(const AppConfig& cfg)
{
    int port = std::stoi(cfg.path_or_port);
    Logger logger(to_abs_path("logs/toy_quant.log"));
    logger.log("[Mode: UDP] Listening on UDP port: ", port, "... strategy=", cfg.strategy_name);

    auto output_files = open_output_files("udp://" + std::to_string(port));
    OrderBook order_book;
    auto strategy = make_strategy(cfg.strategy_name);
    MatchingEngine engine(&logger);
    Portfolio portfolio;
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine,
                      portfolio, logger);
    UdpFeed feed(port);
    feed.start();

    Tick tick;
    while (true)
    {
        if (feed.pop_tick(tick))
        {
            pipeline.process_tick(tick, true);
        }
        else
        {
            _mm_pause();
        }
    }
}

}  // namespace

int main(int argc, char** argv)
{
    AppConfig cfg;
    std::string error;
    if (!parse_config(argc, argv, cfg, error))
    {
        if (!error.empty()) std::cerr << "Error: " << error << "\n";
        print_usage(argv[0]);
        return error.empty() ? 0 : 1;
    }

    try
    {
        if (cfg.mode == "csv")
        {
            run_csv_mode(cfg);
            return 0;
        }

        if (cfg.mode == "udp")
        {
            run_udp_mode(cfg);
            return 0;
        }
        if (cfg.mode == "replay")
        {
            run_replay_mode(cfg);
            return 0;
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }

    print_usage(argv[0]);
    return 1;
}