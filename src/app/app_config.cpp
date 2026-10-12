#include "app/app_config.h"

#include <algorithm>
#include <array>
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
// The accepted strategy names, in one place. The usage text prints this list and the validation below
// searches it, so a name cannot be accepted without being documented: the two were written out
// separately, and four names (`l2_baseline`, `l2_depth`, `l2_micro`, `l2_flow`) were accepted while the
// usage text stopped short of them. The order is the order the usage text prints, grouped by family.
constexpr std::array<std::string_view, 17> kStrategyNames = {
    // The touch-only baselines. Neither implements the L2 entry point, so neither sees depth.
    "optimized", "naive",
    // The frozen L1 line, kept so recorded results stay reproducible rather than re-measured.
    "l1", "passive_l1", "inventory_aware_l1", "flow_aware_l1", "active_l1",
    // The active L2 line. These are one class under several signal configs, which is why the names
    // outnumber the classes they select.
    "l2", "passive_l2", "l2_baseline", "l2_depth", "l2_micro", "l2_flow", "flow_aware_l2",
    "inventory_aware_l2", "active_l2", "adaptive_l2",
};
constexpr std::string_view kDefaultStrategyName = "optimized";

constexpr std::string_view kProfileStagesPrefix = "--profile-stages=";
constexpr std::string_view kLivePacingPrefix = "--live-pacing=";
constexpr std::string_view kRecordPrefix = "--record=";
constexpr std::string_view kFeedThreadFlag = "--feed-thread";
constexpr std::string_view kRingCapacityPrefix = "--ring-capacity=";
constexpr std::string_view kWebSocketTargetPrefix = "--ws-target=";
constexpr std::string_view kWebSocketSecondsPrefix = "--ws-seconds=";
constexpr std::string_view kPaperBrokerFlag = "--paper-broker";
constexpr std::string_view kBrokerRejectPrefix = "--broker-reject-every=";
constexpr std::string_view kBrokerPartialPrefix = "--broker-partial-every=";
constexpr std::string_view kBrokerDropPrefix = "--broker-drop-every=";
constexpr std::string_view kBrokerOutageAfterPrefix = "--broker-outage-after=";
constexpr std::string_view kBrokerOutageHoldsPrefix = "--broker-outage-holds=";
constexpr std::string_view kOrderSizePrefix = "--order-size=";
constexpr std::string_view kInventoryLimitPrefix = "--inventory-limit=";
constexpr std::string_view kMaxPositionPrefix = "--max-position=";
constexpr std::string_view kMaxExposurePrefix = "--max-exposure-usd=";
constexpr std::string_view kKillSwitchPrefix = "--kill-switch-loss-usd=";
constexpr std::string_view kBaseSpreadPrefix = "--base-spread-ticks=";
constexpr std::string_view kInventoryShiftPrefix = "--inventory-shift-ticks=";
constexpr std::string_view kRefreshPrefix = "--refresh-price-ticks=";
constexpr std::string_view kEdgeCoverPrefix = "--edge-cover-ticks=";
constexpr std::string_view kMinSpreadPrefix = "--min-spread-ticks=";
constexpr std::string_view kQueueChunksPrefix = "--queue-chunks=";
constexpr std::string_view kQueueSeedPrefix = "--queue-seed=";
constexpr std::string_view kArrivalSharePrefix = "--arrival-share=";
constexpr std::string_view kOrderLatencyPrefix = "--order-latency-us=";
constexpr std::string_view kCancelLatencyPrefix = "--cancel-latency-us=";
constexpr std::string_view kTraceOrderPrefix = "--trace-order=";
constexpr std::string_view kFundingRatesPrefix = "--funding-rates=";
constexpr std::string_view kSymbolAPrefix = "--symbol-a=";
constexpr std::string_view kSymbolBPrefix = "--symbol-b=";
constexpr std::string_view kNotionalPrefix = "--notional-usd=";
constexpr std::string_view kLongPrefix = "--long=";
constexpr std::string_view kMakerFeePrefix = "--maker-fee-rate=";
constexpr std::string_view kFeeSchedulePrefix = "--fee-schedule=";
constexpr std::string_view kMarkoutCoverFlag = "--markout-cover";
constexpr std::string_view kFlattenBandPrefix = "--flatten-band=";
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
    else if (value == "lumpy") result = QueueModel::Lumpy;
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
                         "[--fast-validation] [--profile-stages[=interval]] [--live-pacing[=speed]]\n";
        std::cerr << "   or: " << executable
                      << " ws <host:port> <symbol> [strategy] [quantity_scale] [queue_model] "
                         "[--ws-target=/path] [--ws-seconds=N] [--no-output] [--fast-validation] "
                         "[--record=PREFIX] [--feed-thread] [--paper-broker]; the live twin of "
                         "l2_replay, reading the same replay shapes over a websocket instead of from "
                         "files\n";
        std::cerr << "   or: " << executable
                      << " carry <leg_a_funding_csv> <leg_b_funding_csv> --symbol-a=SYM --symbol-b=SYM "
                         "[--notional-usd=N] [--long=b]\n";
        std::cerr << "   optional flags: --no-output | --fast-validation (l2_replay only) | "
                     "--profile-stages[=N] prints sampled per-stage timings (l2_replay only, default interval 64) | "
                     "--order-size=N, --inventory-limit=N, and --max-position=N override the "
                     "values derived from the instrument quantity scale | --base-spread-ticks=N "
                     "sets the quote half-spread, 1 being the venue touch\n";
        std::cerr << "   overrides: --inventory-shift-ticks=N shifts the inventory-aware quotes by N ticks "
                     "at a full position, --refresh-price-ticks=N is how far the strategy's own fair price "
                     "has to move before its resting quotes are pulled and replaced, --edge-cover-ticks=N "
                     "requires a quote to cover N ticks on top of the maker fee before it is placed, and "
                     "--min-spread-ticks=N quotes only where the touch is at least N ticks wide; 0 leaves "
                     "each of them at the strategy's own setting\n";
        std::cerr << "   limits: --max-exposure-usd=N is the exposure ceiling the summary measures the run "
                     "against, enforced rather than reported once it is named, and --kill-switch-loss-usd=N "
                     "stops the run when its loss reaches N dollars instead of carrying the position into "
                     "the final mark\n";
        std::cerr << "   strategy: ";
        for (const std::string_view name : kStrategyNames)
        {
            if (name != kStrategyNames.front()) std::cerr << " | ";
            std::cerr << name;
            if (name == kDefaultStrategyName) std::cerr << " (default)";
        }
        std::cerr << "\n";
        std::cerr << "   queue_model: prorata (default, calibrated) | lumpy (same mean, real variance) | "
                     "conservative (lower bound) | optimistic (upper bound)\n";
        std::cerr << "   queue: --queue-chunks=N is how many chunks a level decrease is split into when the "
                     "lumpy model draws from it, and --queue-seed=N is that generator's seed, fixed by "
                     "default so a lumpy run is reproducible\n";
        std::cerr << "   arrival: --arrival-share=X sets the share of the displayed size a new quote "
                     "starts behind; 1.0 is FIFO\n";
        std::cerr << "   funding: --funding-rates=FILE applies the instrument's funding history as the "
                     "replay passes each settlement, reported as a [FUNDING] line\n";
        std::cerr << "   schedule: --fee-schedule=KEY=VALUE[,KEY=VALUE] (l2_replay) prices a run under a "
                     "broker's own schedule, where the keys are min (a floor per order, in USD), regulatory "
                     "(a fraction of the proceeds of a sale), clearing and audit (USD a share on every "
                     "execution) and rebate (USD a share of credit for adding liquidity); every one of them "
                     "is zero unless named, and the [Mode] line reports the five in force";
        std::cerr << "\n";
        std::cerr << "   fees: --maker-fee-rate=R replaces the venue's maker rate for a sensitivity "
                     "measurement, and the run reports it in the [Mode] line\n";
        std::cerr << "   gate: --markout-cover (l2_replay, active_l2) quotes only where the capture also "
                     "covers what passive fills at that distance have cost, measured from the venue's "
                     "prints, and reports [GATE]\n";
        std::cerr << "   band: --flatten-band=N (l2_replay) reduces a position above N units with a taker "
                     "order at the touch instead of carrying it into the final mark\n";
        std::cerr << "   record: --record=PREFIX (l2_replay) writes PREFIX_trades.csv and "
                     "PREFIX_depth.csv in the replay shapes, so the stream this run was fed can be "
                     "replayed again\n";
        std::cerr << "   trace: --trace-order=N reports the pipeline's per-order hints for order N, which "
                     "is how one order's decisions are read back\n";
        std::cerr << "   threads: --feed-thread (l2_replay) reads on its own thread and hands each "
                     "event to the strategy thread over a ring, reporting the hand-off as a "
                     "[LATENCY] line; --ring-capacity=N sizes that ring (default 4096)\n";
        std::cerr << "   latency: --order-latency-us=N and --cancel-latency-us=N hold an order or a cancel "
                     "for N microseconds of the market's own clock before the venue sees it, which is what "
                     "being slow costs and the one part of the order lifecycle a replay can model "
                     "directly\n";
        std::cerr << "   broker: --paper-broker (l2_replay) puts a broker between the strategy and the "
                     "venue that keeps its own statement and reports [BROKER] and [RECON]; "
                     "--broker-reject-every=N refuses every Nth order, --broker-partial-every=N splits "
                     "every Nth fill, --broker-drop-every=N withholds every Nth fill from the strategy, "
                     "and --broker-outage-after=N with --broker-outage-holds=M holds the next M orders "
                     "while the venue is unreachable\n";
        std::cerr << "   --help prints this text\n";
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
    else if (mode == "carry")
        cfg.mode = AppMode::Carry;
    else if (mode == "ws")
        cfg.mode = AppMode::WebSocket;
    else
    {
        error = "mode must be 'csv', 'udp', 'replay', 'l2_replay', 'ws', or 'carry'";
        return false;
    }

    // The two L2 delivery modes. They share every optional flag, the book, the engine, the recorder
    // and the paper broker; they differ only in whether the readers come from files or from a socket,
    // which is why the flag block below treats them as one family rather than naming each flag twice.
    const bool is_l2_family =
        cfg.mode == AppMode::L2Replay || cfg.mode == AppMode::WebSocket;

    if (cfg.mode == AppMode::Replay || is_l2_family)
    {
        // The optional positional arguments differ between the two sources -- a replay takes two files
        // and a symbol, a live session a host:port and a symbol -- so they are parsed here, and every
        // optional flag after them is parsed once, below, for both.
        int first_optional_flag = 0;
        if (cfg.mode == AppMode::WebSocket)
        {
            // ws <host:port> <symbol> [strategy] [quantity_scale] [queue_model]
            if (argc < 4)
            {
                error = "ws requires a host:port endpoint and a symbol";
                return false;
            }
            const std::string endpoint = argv[2];
            const std::size_t colon = endpoint.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size())
            {
                error = "ws endpoint must be host:port";
                return false;
            }
            cfg.ws_host = endpoint.substr(0, colon);
            int port = 0;
            if (!parse_integer(endpoint.substr(colon + 1), port) || port < 1 || port > 65535)
            {
                error = "ws port must be an integer from 1 to 65535";
                return false;
            }
            cfg.ws_port = static_cast<uint16_t>(port);
            cfg.path_or_port = endpoint;  // kept for the runtime header and the [WS] line
            cfg.symbol = argv[3];
            if (cfg.symbol.empty())
            {
                error = "ws symbol cannot be empty";
                return false;
            }
            first_optional_flag = 4;
            if (argc >= 5 && argv[4][0] != '-')
            {
                cfg.strategy_name = argv[4];
                first_optional_flag = 5;
            }
            if (argc >= 6 && argv[5][0] != '-')
            {
                if (!parse_unsigned(argv[5], cfg.quantity_scale) || cfg.quantity_scale == 0)
                {
                    error = "quantity scale must be a positive integer";
                    return false;
                }
                first_optional_flag = 6;
            }
            if (argc >= 7 && argv[6][0] != '-')
            {
                if (!parse_queue_model(argv[6], cfg.queue_model))
                {
                    error = "queue model must be conservative, prorata, lumpy, or optimistic";
                    return false;
                }
                first_optional_flag = 7;
            }
        }
        else
        {
            // No upper bound on the argument count: every optional flag is validated as it is parsed,
            // so capping the total only rejected valid commands with a misleading message once the
            // flags multiplied, which is exactly what happened when two were added at once.
            if (argc < 5)
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
            if (argc >= 8 &&
                (!parse_unsigned(argv[7], cfg.quantity_scale) || cfg.quantity_scale == 0))
            {
                error = "quantity scale must be a positive integer";
                return false;
            }
            // The queue model is optional, so an argument starting with -- is a flag rather than a
            // model name. Without this, `... active_l2 1 --fast-validation` was read as the model
            // being \"--fast-validation\" and failed, which made the optional positional impossible to
            // omit.
            first_optional_flag = 8;
            if (argc >= 9 && argv[8][0] != '-')
            {
                if (!parse_queue_model(argv[8], cfg.queue_model))
                {
                    error = "queue model must be conservative, prorata, lumpy, or optimistic";
                    return false;
                }
                first_optional_flag = 9;
            }
        }
        // The broker's faults are all "every Nth order or fill" counts, so one parser serves them. Each
        // needs the broker itself: without it the number would be silently ignored, which is worse than
        // being refused.
        const auto parse_fault_count = [&cfg, &error, is_l2_family](const std::string& flag,
                                                                    std::string_view prefix,
                                                                    const char* name,
                                                                    uint64_t& target) -> bool
        {
            if (!is_l2_family)
            {
                error = std::string(name) + " is an l2_replay or ws flag, beside --paper-broker";
                return false;
            }
            uint64_t count = 0;
            if (!parse_unsigned(flag.substr(prefix.size()), count) || count == 0)
            {
                error = std::string(name) + " must be a positive integer";
                return false;
            }
            target = count;
            return true;
        };
        for (int argument = first_optional_flag; argument < argc; ++argument)
        {
            const std::string flag = argv[argument];
            if (flag == "--no-output")
                cfg.discard_output = true;
            else if (flag == kMarkoutCoverFlag)
                cfg.markout_cover = true;
            else if (flag.compare(0, kFlattenBandPrefix.size(), kFlattenBandPrefix) == 0)
            {
                uint64_t band = 0;
                if (!parse_unsigned(flag.substr(kFlattenBandPrefix.size()), band) || band == 0)
                {
                    error = "--flatten-band must be a positive integer";
                    return false;
                }
                cfg.flatten_band = static_cast<int64_t>(band);
            }
            else if (flag == "--fast-validation")
                cfg.fast_validation = true;
            else if (flag == "--profile-stages")
                cfg.profile_sample_interval = StageProfiler::kDefaultSampleInterval;
            else if (flag == "--live-pacing")
            {
                cfg.live_pacing = true;
                cfg.live_pacing_speed = 1.0;
            }
            else if (flag.compare(0, kLivePacingPrefix.size(), kLivePacingPrefix) == 0)
            {
                const std::string value = flag.substr(kLivePacingPrefix.size());
                try
                {
                    cfg.live_pacing_speed = std::stod(value);
                }
                catch (const std::exception&)
                {
                    error = "--live-pacing speed must be a number";
                    return false;
                }
                if (!(cfg.live_pacing_speed > 0.0))
                {
                    error = "--live-pacing speed must be positive";
                    return false;
                }
                cfg.live_pacing = true;
            }
            else if (flag.compare(0, kRecordPrefix.size(), kRecordPrefix) == 0)
            {
                // The recorder writes the two streams the L2 replay reads: a trade file and a depth
                // file. The L1 replay's second stream is a BBO quote, and no replay CSV in this
                // project holds one as rows, so asking to record there is refused rather than
                // silently ignored.
                if (!is_l2_family)
                {
                    error = "--record is an l2_replay or ws flag: the L1 replay's second stream is a "
                            "BBO quote, which the replay CSV formats cannot carry as rows";
                    return false;
                }
                if (flag.size() == kRecordPrefix.size())
                {
                    error = "--record needs a file prefix";
                    return false;
                }
                cfg.record_prefix = flag.substr(kRecordPrefix.size());
            }
            else if (flag == kFeedThreadFlag)
            {
                if (!is_l2_family)
                {
                    error = "--feed-thread is an l2_replay or ws flag: the L1 replay's second stream "
                            "is a BBO quote, and a quote is not what this hand-off carries";
                    return false;
                }
                cfg.feed_thread = true;
            }
            else if (flag.compare(0, kWebSocketTargetPrefix.size(), kWebSocketTargetPrefix) == 0)
            {
                if (cfg.mode != AppMode::WebSocket)
                {
                    error = "--ws-target is a ws flag: it is the HTTP path the live handshake asks for";
                    return false;
                }
                const std::string target = flag.substr(kWebSocketTargetPrefix.size());
                if (target.empty() || target.front() != '/')
                {
                    error = "--ws-target must be a path beginning with /";
                    return false;
                }
                cfg.ws_target = target;
            }
            else if (flag.compare(0, kWebSocketSecondsPrefix.size(), kWebSocketSecondsPrefix) == 0)
            {
                if (cfg.mode != AppMode::WebSocket)
                {
                    error = "--ws-seconds is a ws flag: it bounds a live session, which a file replay "
                            "does not need";
                    return false;
                }
                std::istringstream stream(flag.substr(kWebSocketSecondsPrefix.size()));
                if (!(stream >> cfg.ws_seconds) || !(cfg.ws_seconds > 0.0))
                {
                    error = "--ws-seconds must be a positive number of seconds";
                    return false;
                }
            }
            else if (flag.compare(0, kRingCapacityPrefix.size(), kRingCapacityPrefix) == 0)
            {
                if (!is_l2_family)
                {
                    error = "--ring-capacity is an l2_replay or ws flag, beside --feed-thread";
                    return false;
                }
                uint64_t capacity = 0;
                if (!parse_unsigned(flag.substr(kRingCapacityPrefix.size()), capacity) || capacity == 0)
                {
                    error = "--ring-capacity must be a positive integer";
                    return false;
                }
                cfg.ring_capacity = static_cast<std::size_t>(capacity);
            }
            else if (flag == kPaperBrokerFlag)
            {
                if (!is_l2_family)
                {
                    error = "--paper-broker is an l2_replay or ws flag: it wraps that mode's engine";
                    return false;
                }
                cfg.paper_broker = true;
            }
            else if (flag.compare(0, kBrokerRejectPrefix.size(), kBrokerRejectPrefix) == 0)
            {
                if (!parse_fault_count(flag, kBrokerRejectPrefix, "--broker-reject-every",
                                       cfg.broker_reject_every))
                    return false;
            }
            else if (flag.compare(0, kBrokerPartialPrefix.size(), kBrokerPartialPrefix) == 0)
            {
                if (!parse_fault_count(flag, kBrokerPartialPrefix, "--broker-partial-every",
                                       cfg.broker_partial_every))
                    return false;
            }
            else if (flag.compare(0, kBrokerDropPrefix.size(), kBrokerDropPrefix) == 0)
            {
                if (!parse_fault_count(flag, kBrokerDropPrefix, "--broker-drop-every",
                                       cfg.broker_drop_every))
                    return false;
            }
            else if (flag.compare(0, kBrokerOutageAfterPrefix.size(), kBrokerOutageAfterPrefix) == 0)
            {
                if (!parse_fault_count(flag, kBrokerOutageAfterPrefix, "--broker-outage-after",
                                       cfg.broker_outage_after))
                    return false;
            }
            else if (flag.compare(0, kBrokerOutageHoldsPrefix.size(), kBrokerOutageHoldsPrefix) == 0)
            {
                if (!parse_fault_count(flag, kBrokerOutageHoldsPrefix, "--broker-outage-holds",
                                       cfg.broker_outage_holds))
                    return false;
            }
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
            else if (flag.compare(0, kMaxExposurePrefix.size(), kMaxExposurePrefix) == 0)
            {
                double exposure = 0.0;
                std::istringstream stream(flag.substr(kMaxExposurePrefix.size()));
                if (!(stream >> exposure) || !(exposure > 0.0))
                {
                    error = "--max-exposure-usd must be a positive number of US dollars";
                    return false;
                }
                cfg.max_exposure_usd = exposure;
            }
            else if (flag.compare(0, kKillSwitchPrefix.size(), kKillSwitchPrefix) == 0)
            {
                double loss = 0.0;
                std::istringstream stream(flag.substr(kKillSwitchPrefix.size()));
                if (!(stream >> loss) || !(loss > 0.0))
                {
                    error = "--kill-switch-loss-usd must be a positive number of US dollars";
                    return false;
                }
                cfg.kill_switch_loss_usd = loss;
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
            else if (flag.compare(0, kInventoryShiftPrefix.size(), kInventoryShiftPrefix) == 0)
            {
                double ticks = 0.0;
                std::istringstream stream(flag.substr(kInventoryShiftPrefix.size()));
                if (!(stream >> ticks) || !(ticks > 0.0))
                {
                    error = "--inventory-shift-ticks must be a positive number of ticks";
                    return false;
                }
                cfg.inventory_shift_ticks = ticks;
            }
            else if (flag.compare(0, kRefreshPrefix.size(), kRefreshPrefix) == 0)
            {
                uint64_t ticks = 0;
                if (!parse_unsigned(flag.substr(kRefreshPrefix.size()), ticks) || ticks == 0)
                {
                    error = "--refresh-price-ticks must be a positive integer";
                    return false;
                }
                cfg.refresh_price_ticks_override = ticks;
            }
            else if (flag.compare(0, kEdgeCoverPrefix.size(), kEdgeCoverPrefix) == 0)
            {
                double ticks = 0.0;
                std::istringstream stream(flag.substr(kEdgeCoverPrefix.size()));
                if (!(stream >> ticks) || ticks < 0.0)
                {
                    error = "--edge-cover-ticks must not be negative";
                    return false;
                }
                cfg.edge_cover_ticks = ticks;
            }
            else if (flag.compare(0, kMinSpreadPrefix.size(), kMinSpreadPrefix) == 0)
            {
                double ticks = 0.0;
                std::istringstream stream(flag.substr(kMinSpreadPrefix.size()));
                if (!(stream >> ticks) || ticks < 0.0)
                {
                    error = "--min-spread-ticks must not be negative";
                    return false;
                }
                cfg.min_spread_ticks = ticks;
            }
            else if (flag.compare(0, kQueueChunksPrefix.size(), kQueueChunksPrefix) == 0)
            {
                uint64_t chunks = 0;
                if (!parse_unsigned(flag.substr(kQueueChunksPrefix.size()), chunks) ||
                    chunks == 0)
                {
                    error = "--queue-chunks must be a positive integer";
                    return false;
                }
                cfg.queue_lump_chunks = chunks;
            }
            else if (flag.compare(0, kQueueSeedPrefix.size(), kQueueSeedPrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kQueueSeedPrefix.size()), cfg.queue_seed))
                {
                    error = "--queue-seed must be a non-negative integer";
                    return false;
                }
            }
            else if (flag.compare(0, kArrivalSharePrefix.size(), kArrivalSharePrefix) == 0)
            {
                double share = 0.0;
                std::istringstream stream(flag.substr(kArrivalSharePrefix.size()));
                if (!(stream >> share) || share < 0.0 || share > 1.0)
                {
                    error = "--arrival-share must be a number between 0 and 1";
                    return false;
                }
                cfg.arrival_share = share;
            }
            else if (flag.compare(0, kOrderLatencyPrefix.size(), kOrderLatencyPrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kOrderLatencyPrefix.size()), cfg.order_latency_us))
                {
                    error = "--order-latency-us must be a non-negative integer";
                    return false;
                }
            }
            else if (flag.compare(0, kCancelLatencyPrefix.size(), kCancelLatencyPrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kCancelLatencyPrefix.size()), cfg.cancel_latency_us))
                {
                    error = "--cancel-latency-us must be a non-negative integer";
                    return false;
                }
            }
            else if (flag.compare(0, kTraceOrderPrefix.size(), kTraceOrderPrefix) == 0)
            {
                if (!parse_unsigned(flag.substr(kTraceOrderPrefix.size()), cfg.trace_order))
                {
                    error = "--trace-order must be a non-negative integer";
                    return false;
                }
            }
            else if (flag.compare(0, kFundingRatesPrefix.size(), kFundingRatesPrefix) == 0)
            {
                cfg.funding_rates_path = flag.substr(kFundingRatesPrefix.size());
                if (cfg.funding_rates_path.empty())
                {
                    error = "--funding-rates needs a file path";
                    return false;
                }
            }
            else if (flag.compare(0, kMakerFeePrefix.size(), kMakerFeePrefix) == 0)
            {
                std::istringstream stream(flag.substr(kMakerFeePrefix.size()));
                if (!(stream >> cfg.maker_fee_rate))
                {
                    error = "--maker-fee-rate must be a number, and may be negative for a rebate";
                    return false;
                }
                cfg.has_maker_fee_rate = true;
            }
            else if (flag.compare(0, kFeeSchedulePrefix.size(), kFeeSchedulePrefix) == 0)
            {
                // Comma separated key=value pairs, because the terms arrive together: one flag that names all
                // five keeps a sensitivity run to one command line, and a run that names a subset is priced
                // under that subset with the rest at zero rather than under a half remembered default.
                std::stringstream items(flag.substr(kFeeSchedulePrefix.size()));
                std::string item;
                while (std::getline(items, item, ','))
                {
                    const std::size_t equals = item.find('=');
                    if (equals == std::string::npos)
                    {
                        error = "--fee-schedule wants key=value pairs separated by commas";
                        return false;
                    }
                    const std::string key = item.substr(0, equals);
                    double value = 0.0;
                    std::istringstream value_stream(item.substr(equals + 1));
                    if (!(value_stream >> value))
                    {
                        error = "--fee-schedule value for " + key + " must be a number";
                        return false;
                    }
                    if (key == "min")
                        cfg.fee_schedule.min_commission_per_order = value;
                    else if (key == "regulatory")
                        cfg.fee_schedule.regulatory_fee_rate = value;
                    else if (key == "clearing")
                        cfg.fee_schedule.clearing_fee_per_share = value;
                    else if (key == "audit")
                        cfg.fee_schedule.audit_fee_per_share = value;
                    else if (key == "rebate")
                        cfg.fee_schedule.maker_rebate_per_share = value;
                    else
                    {
                        error = "--fee-schedule keys are min, regulatory, clearing, audit and rebate";
                        return false;
                    }
                }
                cfg.has_fee_schedule = true;
            }
            else
            {
                error = "optional replay flags must be --no-output, --fast-validation, "
                        "--profile-stages[=interval], --live-pacing[=speed], --order-size=N, --inventory-limit=N, "
                        "--max-position=N, --base-spread-ticks=N, --refresh-price-ticks=N, "
                        "--queue-chunks=N, --queue-seed=N, --arrival-share=X, --order-latency-us=N, "
                        "--cancel-latency-us=N, --trace-order=N, --funding-rates=FILE, --maker-fee-rate=R, "
                        "--fee-schedule=KEY=VALUE[,KEY=VALUE], "
                        "--markout-cover, --flatten-band=N, or --paper-broker with one of its faults";
                return false;
            }
        }
        if (cfg.fast_validation && !is_l2_family)
        {
            error = "--fast-validation is only supported by l2_replay or ws";
            return false;
        }
        if (cfg.profile_sample_interval > 0 && !is_l2_family)
        {
            error = "--profile-stages is only supported by l2_replay or ws";
            return false;
        }
        if (cfg.live_pacing && cfg.mode != AppMode::L2Replay)
        {
            // A live socket is paced by the venue, not by a file's clock, so pacing has no meaning
            // there and is refused rather than silently ignored.
            error = "--live-pacing is only supported by l2_replay";
            return false;
        }
        if (cfg.markout_cover && !is_l2_family)
        {
            error = "--markout-cover is only supported by l2_replay or ws";
            return false;
        }
        if (cfg.flatten_band > 0 && !is_l2_family)
        {
            error = "--flatten-band is only supported by l2_replay or ws, whose decision layer carries "
                    "the band";
            return false;
        }
        if (!cfg.paper_broker &&
            (cfg.broker_reject_every > 0 || cfg.broker_partial_every > 0 || cfg.broker_drop_every > 0 ||
             cfg.broker_outage_after > 0 || cfg.broker_outage_holds > 0))
        {
            // Silently ignoring a fault would make a faulted run and a clean run indistinguishable in
            // the summary, which is the one thing a fault injection must never do.
            error = "a --broker-* fault needs --paper-broker, the broker it is a fault of";
            return false;
        }
        if (cfg.broker_outage_holds > 0 && cfg.broker_outage_after == 0)
        {
            error = "--broker-outage-holds needs --broker-outage-after: nothing says when the venue goes";
            return false;
        }
        if (cfg.markout_cover && cfg.strategy_name != "active_l2" &&
            cfg.strategy_name != "adaptive_l2")
        {
            error = "--markout-cover needs a strategy whose decision layer carries the gate: active_l2 or "
                    "its adaptive_l2 alias";
            return false;
        }
    }
    else if (cfg.mode == AppMode::Carry)
    {
        // Two funding histories and the position they are measured with. There is no book file and no
        // strategy, so every other input is a flag and this branch owns all of them.
        if (argc < 4)
        {
            error = "carry requires two funding rate files";
            return false;
        }
        cfg.path_or_port = argv[2];
        cfg.funding_b_path = argv[3];
        for (int argument = 4; argument < argc; ++argument)
        {
            const std::string flag = argv[argument];
            if (flag.compare(0, kSymbolAPrefix.size(), kSymbolAPrefix) == 0)
                cfg.symbol = flag.substr(kSymbolAPrefix.size());
            else if (flag.compare(0, kSymbolBPrefix.size(), kSymbolBPrefix) == 0)
                cfg.symbol_b = flag.substr(kSymbolBPrefix.size());
            else if (flag.compare(0, kNotionalPrefix.size(), kNotionalPrefix) == 0)
            {
                std::istringstream stream(flag.substr(kNotionalPrefix.size()));
                if (!(stream >> cfg.carry_notional_usd) || !(cfg.carry_notional_usd > 0.0))
                {
                    error = "--notional-usd must be a positive number";
                    return false;
                }
            }
            else if (flag.compare(0, kLongPrefix.size(), kLongPrefix) == 0)
            {
                const std::string leg = flag.substr(kLongPrefix.size());
                if (leg == "a")
                    cfg.carry_leg_a_long = true;
                else if (leg == "b")
                    cfg.carry_leg_a_long = false;
                else
                {
                    error = "--long must be a or b";
                    return false;
                }
            }
            else
            {
                error = "carry flags must be --symbol-a=SYM, --symbol-b=SYM, --notional-usd=N, or "
                        "--long=a|b";
                return false;
            }
        }
        if (cfg.symbol.empty() || cfg.symbol_b.empty())
        {
            error = "carry needs --symbol-a and --symbol-b, because the venue's fee schedule is looked up "
                    "by symbol and not passed on the command line";
            return false;
        }
        if (cfg.symbol == cfg.symbol_b)
        {
            error = "carry needs two different symbols: the same one on both legs would net into a single "
                    "position rather than a hedge";
            return false;
        }
        for (const std::string& path : {cfg.path_or_port, cfg.funding_b_path})
        {
            if (!std::filesystem::is_regular_file(to_abs_path(path)))
            {
                error = "carry funding file does not exist: " + to_abs_path(path);
                return false;
            }
        }
        return true;
    }
    else if (argc > 5 || (cfg.mode == AppMode::LegacyUdp && argc > 4))
    {
        error = "too many arguments";
        return false;
    }

    const bool is_replay_mode =
        cfg.mode == AppMode::Replay || cfg.mode == AppMode::L2Replay ||
        cfg.mode == AppMode::WebSocket;
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

    // One list, searched rather than spelled out: the names this accepts and the names the usage text
    // prints are the same array, so they cannot drift apart again.
    const std::string_view requested_strategy = cfg.strategy_name;
    if (std::find(kStrategyNames.begin(), kStrategyNames.end(), requested_strategy) ==
        kStrategyNames.end())
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
