#include "app/app_config.h"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif

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
    std::cerr << "   strategy: optimized (default) | naive | l1 | passive_l1 | inventory_aware_l1 | flow_aware_l1 | active_l1\n";
}

bool parse_config(int argc, char** argv, AppConfig& cfg, std::string& error)
{
    if (argc == 2 && std::string(argv[1]) == "--help") return false;

    const std::string mode = argc >= 2 ? argv[1] : "csv";
    if (mode == "csv")
        cfg.mode = AppMode::LegacyCsv;
    else if (mode == "udp")
        cfg.mode = AppMode::LegacyUdp;
    else if (mode == "replay")
        cfg.mode = AppMode::Replay;
    else
    {
        error = "mode must be 'csv', 'udp', or 'replay'";
        return false;
    }

    if (cfg.mode == AppMode::Replay)
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
    else if (argc > 5 || (cfg.mode == AppMode::LegacyUdp && argc > 4))
    {
        error = "too many arguments";
        return false;
    }

    if (cfg.mode != AppMode::Replay && argc >= 3) cfg.path_or_port = argv[2];
    if (cfg.mode == AppMode::LegacyCsv && cfg.path_or_port.empty())
    {
        error = "CSV path cannot be empty";
        return false;
    }
    if (cfg.mode == AppMode::LegacyCsv &&
        !std::filesystem::is_regular_file(to_abs_path(cfg.path_or_port)))
    {
        error = "CSV file does not exist: " + to_abs_path(cfg.path_or_port);
        return false;
    }

    if (cfg.mode != AppMode::Replay && argc >= 4)
    {
        if (cfg.mode == AppMode::LegacyUdp)
        {
            cfg.strategy_name = argv[3];
        }
        else if (!parse_integer(argv[3], cfg.delay) || cfg.delay < 0)
        {
            error = "delay must be a non-negative integer";
            return false;
        }
    }
    if (cfg.mode != AppMode::Replay && argc >= 5) cfg.strategy_name = argv[4];

    if (cfg.strategy_name != "optimized" && cfg.strategy_name != "naive" &&
        cfg.strategy_name != "l1" && cfg.strategy_name != "passive_l1" &&
        cfg.strategy_name != "inventory_aware_l1" && cfg.strategy_name != "flow_aware_l1" &&
        cfg.strategy_name != "active_l1")
    {
        error = "strategy must be 'optimized', 'naive', 'l1', 'passive_l1', 'inventory_aware_l1', 'flow_aware_l1', or 'active_l1'";
        return false;
    }

    if (cfg.mode == AppMode::LegacyUdp)
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
