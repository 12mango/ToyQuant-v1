#include <charconv>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "app/strategy_benchmark.h"

namespace
{
// What this driver can be told. Fifteen positional arguments became fifteen named ones, and this is why: ten
// of the fifteen existed only to reach the fifteenth, which is what `docs/USER_GUIDE.md` shows -- a command
// line that spells out ten default values so that it can pass a timeline path. The numbers are not repeated
// here: `--help` prints them out of a default-constructed `L1StrategyConfig`, so the text cannot drift from
// what the frozen L1 strategies actually read.
struct Options
{
    std::string trades_path{"data/v2/fixtures/test_aggTrades_5k.csv"};
    std::string quotes_path{"data/v2/fixtures/test_bookTicker_5k.csv"};
    std::string symbol{"BTCUSDT"};
    uint64_t quantity_scale{1000000};
    L1StrategyConfig l1{};
    std::string timeline_path;
};

void print_usage(const char* executable)
{
    const Options defaults;
    const L1StrategyConfig& l1 = defaults.l1;
    std::cerr << "Usage: " << executable
              << " [--trades=PATH] [--quotes=PATH] [symbol and quantity options] [L1 options]\n"
              << "   input:  --trades=PATH (default " << defaults.trades_path << ")\n"
              << "           --quotes=PATH (default " << defaults.quotes_path << ")\n"
              << "           --symbol=SYM (default " << defaults.symbol << ")\n"
              << "           --quantity-scale=N (default " << defaults.quantity_scale << ")\n"
              << "           --timeline=PATH writes an hourly checkpoint per strategy (default: off)\n"
              << "   L1 settings, which the frozen L1 strategies read and no other strategy does:\n"
              << "           --l1-risk-threshold=X (default " << l1.risk_threshold << ")\n"
              << "           --l1-stress-spread-multiplier=X (default " << l1.stress_spread_multiplier
              << ")\n"
              << "           --l1-minimum-stress-quantity-ratio=X (default "
              << l1.minimum_stress_quantity_ratio << ")\n"
              << "           --l1-fee-spread-multiplier=X (default " << l1.fee_spread_multiplier << ")\n"
              << "           --flow-trade-weight=X (default " << l1.flow_config.trade_weight << ")\n"
              << "           --flow-book-weight=X (default " << l1.flow_config.book_weight << ")\n"
              << "           --flow-price-threshold=X (default " << l1.flow_config.price_threshold << ")\n"
              << "           --flow-quantity-threshold=X (default " << l1.flow_config.quantity_threshold
              << ")\n"
              << "           --flow-max-quantity-reduction=X (default "
              << l1.flow_config.max_quantity_reduction << ")\n"
              << "           --flow-price-ticks=N (default " << l1.flow_config.price_ticks << ")\n"
              << "   --help prints this text\n";
}

// `--flag=value` in one argument, the shape `parse_config` takes for the main binary.
bool value_of(const std::string& argument, std::string_view prefix, std::string& value)
{
    if (argument.rfind(prefix, 0) != 0) return false;
    value.assign(argument.substr(prefix.size()));
    return true;
}

template <typename T>
bool parse_number(const std::string& text, T& result)
{
    if (text.empty()) return false;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

// One numeric option. False means the argument is not this option at all; true means it is, including when
// the value is unusable, which is reported through `error` -- so the chain below stays a chain of names
// rather than eleven copies of the same six lines. `minimum` is inclusive, which is how a zero quantity
// scale is refused while a zero flow weight is allowed to mean what it says.
template <typename T>
bool read_number_option(const std::string& argument, std::string_view prefix, T& target,
                        std::string& error, T minimum)
{
    std::string value;
    if (!value_of(argument, prefix, value)) return false;
    if (!parse_number(value, target) || target < minimum)
    {
        // The prefix carries the '='; the message names the option without it.
        error = "invalid " + std::string(prefix.substr(0, prefix.size() - 1)) + " value '" + value +
                "'";
    }
    return true;
}

bool parse_options(int argc, char** argv, Options& options, std::string& error)
{
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        std::string value;
        if (argument == "--help") return false;
        else if (value_of(argument, "--trades=", value)) options.trades_path = value;
        else if (value_of(argument, "--quotes=", value)) options.quotes_path = value;
        else if (value_of(argument, "--symbol=", value)) options.symbol = value;
        else if (value_of(argument, "--timeline=", value)) options.timeline_path = value;
        else if (read_number_option(argument, "--quantity-scale=", options.quantity_scale, error,
                                    uint64_t{1}) ||
                 read_number_option(argument, "--l1-risk-threshold=", options.l1.risk_threshold, error,
                                    0.0) ||
                 read_number_option(argument, "--l1-stress-spread-multiplier=",
                                    options.l1.stress_spread_multiplier, error, 0.0) ||
                 read_number_option(argument, "--l1-minimum-stress-quantity-ratio=",
                                    options.l1.minimum_stress_quantity_ratio, error, 0.0) ||
                 read_number_option(argument, "--l1-fee-spread-multiplier=",
                                    options.l1.fee_spread_multiplier, error, 0.0) ||
                 read_number_option(argument, "--flow-trade-weight=",
                                    options.l1.flow_config.trade_weight, error, 0.0) ||
                 read_number_option(argument, "--flow-book-weight=", options.l1.flow_config.book_weight,
                                    error, 0.0) ||
                 read_number_option(argument, "--flow-price-threshold=",
                                    options.l1.flow_config.price_threshold, error, 0.0) ||
                 read_number_option(argument, "--flow-quantity-threshold=",
                                    options.l1.flow_config.quantity_threshold, error, 0.0) ||
                 read_number_option(argument, "--flow-max-quantity-reduction=",
                                    options.l1.flow_config.max_quantity_reduction, error, 0.0) ||
                 read_number_option(argument, "--flow-price-ticks=", options.l1.flow_config.price_ticks,
                                    error, uint64_t{0}))
        {
            if (!error.empty()) return false;
        }
        else
        {
            error = "unknown option '" + argument + "' (try --help)";
            return false;
        }
    }
    return true;
}
}  // namespace

int main(int argc, char** argv)
{
    Options options;
    std::string error;
    if (!parse_options(argc, argv, options, error))
    {
        if (!error.empty()) std::cerr << "Error: " << error << "\n";
        print_usage(argv[0]);
        return error.empty() ? 0 : 1;
    }

    try
    {
        const auto results =
            run_strategy_benchmark(options.trades_path, options.quotes_path, options.symbol,
                                   options.quantity_scale, options.l1, options.timeline_path);
        std::cout << std::left << std::setw(20) << "strategy" << std::setw(12) << "orders"
                  << std::setw(12) << "filled_qty" << std::setw(10) << "fills"
                  << std::setw(12) << "filtered" 
                  << std::setw(10) << "bbo_pct" << std::setw(10) << "avg_gap"
                  << std::setw(10) << "cancel_nf" << std::setw(10) << "avg_life"
                  << std::setw(12) << "position" << std::setw(14) << "gross_pnl"
                  << std::setw(14) << "net_pnl" << std::setw(12) << "fees"
                      << std::setw(12) << "fee_ratio" << std::setw(14) << "edge"
                      << std::setw(14) << "markout" << std::setw(12) << "avg_inv"
                      << std::setw(12) << "max_inv" << std::setw(10) << "inv_flip" << '\n';
        for (const auto& r : results)
        {
            std::cout << std::left << std::setw(20) << r.strategy_name << std::setw(12)
                      << r.submitted_orders << std::setw(12) << r.filled_quantity << std::setw(10)
                      << r.trade_reports << std::setw(12) << r.filtered_market_trades
                        << std::setw(10)
                        << (r.quote_observations == 0
                            ? 0.0
                            : static_cast<double>(r.bbo_quote_observations) /
                                static_cast<double>(r.quote_observations))
                        << std::setw(10)
                        << (r.quote_observations == 0
                            ? 0.0
                            : r.total_quote_distance /
                                static_cast<double>(r.quote_observations))
                        << std::setw(10) << r.cancelled_before_fill_orders
                            << std::setw(10)
                            << (r.quote_observations == 0
                                ? 0.0
                                : static_cast<double>(r.total_order_lifetime_cycles) /
                                    static_cast<double>(r.quote_observations))
                      << std::setw(12) << r.net_position << std::setw(14)
                      << std::fixed << std::setprecision(6) << r.gross_pnl << std::setw(14)
                      << std::fixed << std::setprecision(6) << r.equity - 1000.0 << std::setw(12)
                          << std::fixed << std::setprecision(6) << r.fees_paid << std::setw(12)
                          << r.fee_ratio << std::setw(14) << r.captured_edge << std::setw(14)
                          << r.adverse_selection << std::setw(12) << r.average_abs_inventory
                          << std::setw(12) << r.max_abs_inventory << std::setw(10)
                          << r.inventory_sign_changes << '\n';
        }
        std::cerr << "flow_config trade_weight=" << options.l1.flow_config.trade_weight
                  << " book_weight=" << options.l1.flow_config.book_weight
                  << " price_threshold=" << options.l1.flow_config.price_threshold
                  << " quantity_threshold=" << options.l1.flow_config.quantity_threshold
                  << " max_quantity_reduction=" << options.l1.flow_config.max_quantity_reduction
                  << " price_ticks=" << options.l1.flow_config.price_ticks << '\n';
        if (!results.empty() && results.front().dislocated_trades > 0)
            std::cerr << "benchmark warning: " << results.front().dislocated_trades
                      << " trades exceed 5 bps from the latest BBO; max deviation="
                      << std::fixed << std::setprecision(4)
                      << results.front().max_trade_deviation_bps << " bps\n";
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "benchmark error: " << ex.what() << '\n';
        return 1;
    }
}
