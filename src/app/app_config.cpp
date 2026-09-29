#include "app/app_config.h"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

#include "utils/stage_profiler.h"

#ifndef PROJECT_ROOT_DIR
#define PROJECT_ROOT_DIR "."
#endif

namespace
{
constexpr std::string_view kProfileStagesPrefix = "--profile-stages=";
constexpr std::string_view kOrderSizePrefix = "--order-size=";
constexpr std::string_view kInventoryLimitPrefix = "--inventory-limit=";
constexpr std::string_view kMaxPositionPrefix = "--max-position=";
constexpr std::string_view kBaseSpreadPrefix = "--base-spread-ticks=";
}  // namespace

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

bool parse_queue_model(const std::string& value, QueueModel& result)
{
    if (value == "conservative") result = QueueModel::Conservative;
    else if (value == "prorata") result = QueueModel::ProRata;
    else if (value == "optimistic") result = QueueModel::Optimistic;
    else return false;
    return true;
}

void print_usage(const char* executable)
{
    std::cerr << "Usage: " << executable << " csv [path_to_csv] [ms_delay] [strategy]\n";
    std::cerr << "   or: " << executable << " udp <port> [strategy]\n";
        std::cerr << "   or: " << executable
              << " replay <agg_trades_csv> <bbo_csv> <symbol> [ms_delay] [strategy] "
                  "[quantity_scale] [queue_model] [--no-output] [--fast-validation]\n";
        std::cerr << "   or: " << executable
                      << " l2_replay <trades_csv[.gz]> <depth_or_incremental_csv> <symbol> "
                         "[ms_delay] [strategy] [quantity_scale] [queue_model] [--no-output] "
                         "[--fast-validation] [--profile-stages[=interval]]\n";
        std::cerr << "   optional flags: --no-output | --fast-validation (l2_replay only) | "
                     "--profile-stages[=N] prints sampled per-stage timings (l2_replay only, default interval 64) | "
                     "--order-size=N, --inventory-limit=N, and --max-position=N override the "
                     "values derived from the instrument quantity scale | --base-spread-ticks=N "
                     "sets the quote half-spread, 1 being the venue touch\n";
        std::cerr << "   strategy: optimized (default) | naive | l1 | passive_l1 | inventory_aware_l1 | flow_aware_l1 | active_l1 | passive_l2 | inventory_aware_l2 | flow_aware_l2 | active_l2 | adaptive_l2 | l2\n";
        std::cerr << "   queue_model: prorata (default, calibrated) | conservative (lower bound) | "
                     "optimistic (upper bound)\n";
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
    else if (mode == "l2_replay")
        cfg.mode = AppMode::L2Replay;
    else
    {
        error = "mode must be 'csv', 'udp', 'replay', or 'l2_replay'";
        return false;
    }

    if (cfg.mode == AppMode::Replay || cfg.mode == AppMode::L2Replay)
    {
        if (argc < 5 || argc > 12)
        {
            error = "replay requires trade file, market-state file, and symbol";
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
        // The queue model is optional, so an argument starting with -- is a flag rather than a model
        // name. Without this, `... active_l2 1 --fast-validation` was read as the model being
        // "--fast-validation" and failed, which made the optional positional impossible to omit.
        int first_optional_flag = 8;
        if (argc >= 9 && argv[8][0] != '-')
        {
            if (!parse_queue_model(argv[8], cfg.queue_model))
            {
                error = "queue model must be conservative, prorata, or optimistic";
                return false;
            }
            first_optional_flag = 9;
        }
        for (int argument = first_optional_flag; argument < argc; ++argument)
        {
            const std::string flag = argv[argument];
            if (flag == "--no-output")
                cfg.discard_output = true;
            else if (flag == "--fast-validation")
                cfg.fast_validation = true;
            else if (flag == "--profile-stages")
                cfg.profile_sample_interval = StageProfiler::kDefaultSampleInterval;
            else if (flag.compare(0, kProfileStagesPrefix.size(), kProfileStagesPrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kProfileStagesPrefix.size()),
                                    cfg.profile_sample_interval) ||
                    cfg.profile_sample_interval == 0)
                {
                    error = "--profile-stages interval must be a positive integer";
                    return false;
                }
            }
            else if (flag.compare(0, kOrderSizePrefix.size(), kOrderSizePrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kOrderSizePrefix.size()), cfg.order_size_override) ||
                    cfg.order_size_override == 0)
                {
                    error = "--order-size must be a positive integer";
                    return false;
                }
            }
            else if (flag.compare(0, kInventoryLimitPrefix.size(), kInventoryLimitPrefix) == 0)
            {
                uint64_t limit = 0;
                if (!parse_unsigned(flag.substr(kInventoryLimitPrefix.size()), limit) || limit == 0)
                {
                    error = "--inventory-limit must be a positive integer";
                    return false;
                }
                cfg.inventory_limit_override = static_cast<int64_t>(limit);
            }
            else if (flag.compare(0, kMaxPositionPrefix.size(), kMaxPositionPrefix) == 0)
            {
                uint64_t limit = 0;
                if (!parse_unsigned(flag.substr(kMaxPositionPrefix.size()), limit) || limit == 0)
                {
                    error = "--max-position must be a positive integer";
                    return false;
                }
                cfg.position_limit = static_cast<int64_t>(limit);
            }
            else if (flag.compare(0, kBaseSpreadPrefix.size(), kBaseSpreadPrefix) == 0)
            {
                double ticks = 0.0;
                std::istringstream stream(flag.substr(kBaseSpreadPrefix.size()));
                if (!(stream >> ticks) || !(ticks > 0.0))
                {
                    error = "--base-spread-ticks must be a positive number of ticks";
                    return false;
                }
                cfg.base_spread_ticks = ticks;
            }
            else
            {
                error = "optional replay flags must be --no-output, --fast-validation, "
                        "--profile-stages[=interval], --order-size=N, --inventory-limit=N, "
                        "--max-position=N, or --base-spread-ticks=N";
                return false;
            }
        }
        if (cfg.fast_validation && cfg.mode != AppMode::L2Replay)
        {
            error = "--fast-validation is only supported by l2_replay";
            return false;
        }
        if (cfg.profile_sample_interval > 0 && cfg.mode != AppMode::L2Replay)
        {
            error = "--profile-stages is only supported by l2_replay";
            return false;
        }
    }
    else if (argc > 5 || (cfg.mode == AppMode::LegacyUdp && argc > 4))
    {
        error = "too many arguments";
        return false;
    }

    const bool is_replay_mode = cfg.mode == AppMode::Replay || cfg.mode == AppMode::L2Replay;
    if (!is_replay_mode && argc >= 3) cfg.path_or_port = argv[2];
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

    if (!is_replay_mode && argc >= 4)
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
    if (!is_replay_mode && argc >= 5) cfg.strategy_name = argv[4];

    if (cfg.strategy_name != "optimized" && cfg.strategy_name != "naive" &&
        cfg.strategy_name != "l1" && cfg.strategy_name != "passive_l1" &&
        cfg.strategy_name != "inventory_aware_l1" && cfg.strategy_name != "flow_aware_l1" &&
        cfg.strategy_name != "active_l1" && cfg.strategy_name != "l2" &&
        cfg.strategy_name != "passive_l2" && cfg.strategy_name != "inventory_aware_l2" &&
        cfg.strategy_name != "flow_aware_l2" && cfg.strategy_name != "adaptive_l2" &&
        cfg.strategy_name != "active_l2" &&
        cfg.strategy_name != "l2_baseline" && cfg.strategy_name != "l2_depth" &&
        cfg.strategy_name != "l2_micro" && cfg.strategy_name != "l2_flow")
    {
        error = "unsupported strategy name";
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
