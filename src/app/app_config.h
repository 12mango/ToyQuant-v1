#pragma once

#include <cstdint>
#include <string>

enum class AppMode
{
    LegacyCsv,
    LegacyUdp,
    Replay,
    L2Replay
};

struct AppConfig
{
    AppMode mode = AppMode::LegacyCsv;
    std::string path_or_port = "data/scenarios/synthetic_ticks.csv";
    std::string quotes_path;
    std::string symbol;
    int delay = 0;
    std::string strategy_name = "optimized";
    uint64_t quantity_scale = 1000000;
};

std::string to_abs_path(const std::string& input_path);
bool parse_config(int argc, char** argv, AppConfig& cfg, std::string& error);
void print_usage(const char* executable);
