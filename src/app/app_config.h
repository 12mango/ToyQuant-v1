#pragma once

#include <cstdint>
#include <string>

#include "common/types.h"

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
    // The calibrated model, not the skeptical one. tools/calibrate_queue_model.py measures 99.96% of
    // the size that leaves a book level as cancel or amendment rather than trade, so the only free
    // parameter left is where in the queue those cancels sit, and pro-rata is the neutral assumption
    // about that. `conservative` (nothing comes off the queue ahead) and `optimistic` (all of it does)
    // bracket it and remain available as the sensitivity bounds they always were.
    QueueModel queue_model = QueueModel::ProRata;
    bool discard_output = false;
    bool fast_validation = false;
    // 0 keeps the value derived from the instrument spec, which is what every earlier run used:
    // order_size = quantity_scale/1000, inventory_limit = quantity_scale/10. Those derivations read
    // as if quantity_scale encoded an economic scale, but for an instrument whose quantities are
    // already contract counts it does not, and BTC-PERPETUAL ends up with an order size of one
    // contract and an inventory limit of one. These overrides let a run model a size that does not
    // come from the quantity scale, without changing any existing command line.
    uint64_t order_size_override = 0;
    int64_t inventory_limit_override = 0;
    // Pre-trade position limit in quantity units for the pipeline gate. 0 leaves the gate off, which
    // is the historical behaviour; the run still reports the marked exposure against the collateral.
    int64_t position_limit = 0;
    // Quote placement, as a half-spread in ticks: 1 puts the quote at the venue touch, 2 is the
    // default the recorded runs use. 0 means "use the default". It exists because the fill count is
    // dominated by where the quote sits, and that has to be measurable rather than argued about.
    double base_spread_ticks = 0.0;
    // 0 disables per-stage profiling. Otherwise this is the sampling interval in events,
    // so 64 times the 64th event of every 64.
    uint64_t profile_sample_interval = 0;
};

std::string to_abs_path(const std::string& input_path);
bool parse_config(int argc, char** argv, AppConfig& cfg, std::string& error);
void print_usage(const char* executable);
