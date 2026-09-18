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

#include "backtest/backtest_driver.h"
#include "backtest/performance.h"
#include "common/instrument_spec.h"
#include "exchange/matching_engine.h"
#include "market/csv_feed.h"
#include "market/replay_feed.h"
#include "market/udp_feed.h"
#include "orderbook/orderbook.h"
#include "strategy/market_maker.h"
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

std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name,
                                        const InstrumentSpec* instrument = nullptr)
{
    const uint64_t order_size =
        instrument ? std::max(instrument->min_order_quantity, instrument->quantity_scale / 1000)
                   : 100;
    const double tick_size = instrument ? instrument->tick_size : PRICE_TICK_SIZE;
    const double spread = instrument ? 2.0 * instrument->tick_size : 0.000003;
    if (strategy_name == "naive")
    {
        return std::make_unique<NaiveMarketMaker>(order_size, spread, tick_size);
    }
    const int64_t inventory_limit =
        instrument ? static_cast<int64_t>(instrument->quantity_scale / 10) : 1000;
    if (strategy_name == "l1")
    {
        L1MarketMakerConfig l1_config;
        l1_config.order_size = order_size;
        l1_config.base_spread = spread;
        l1_config.inventory_limit = inventory_limit;
        l1_config.tick_size = tick_size;
        return std::make_unique<L1MarketMaker>(l1_config);
    }
    return std::make_unique<OptimizedMarketMaker>(order_size, spread, inventory_limit, tick_size);
}

std::string side_to_csv(Side side)
{
    return side == Side::Buy ? "B" : side == Side::Sell ? "S" : "N";
}

exchange::Order to_exchange_order(const StrategyOrder& strategy_order, uint64_t ts,
                                  const std::string& owner)
{
    exchange::Order order{};
    order.id = strategy_order.order_id;
    order.symbol = strategy_order.symbol;
    order.side = strategy_order.side == Side::Buy ? exchange::Side::Buy : exchange::Side::Sell;
    order.type = exchange::OrderType::Limit;
    order.price = strategy_order.price;
    order.qty = strategy_order.quantity;
    order.remaining = strategy_order.quantity;
    order.ts = ts;
    order.owner = owner;
    return order;
}

void write_order_csv_row(std::ofstream& out, uint64_t ts, const StrategyOrder& order)
{
    out << ts << "," << order.symbol << "," << side_to_csv(order.side) << "," << order.price << ","
        << order.quantity << "," << order.order_id << "\n";
}

void write_trade_csv_row(std::ofstream& out, const ExecutionReport& report)
{
    if (report.exec_type != ExecType::Trade || report.owner != "MarketMaker") return;
    const char* role = report.liquidity_role == LiquidityRole::Maker   ? "maker"
                       : report.liquidity_role == LiquidityRole::Taker ? "taker"
                                                                       : "unknown";
    out << report.ts << "," << report.symbol << ","
        << (report.side == exchange::Side::Buy ? "B" : "S") << "," << report.price << ","
        << report.quantity << "," << report.order_id << "," << role << "," << report.fee << "\n";
}

void print_tick(Logger& logger, const Tick& t, const TopOfBook& top)
{
    logger.log("[TICK] ", t.symbol, " ts:", t.ts, " price:", t.price, " size:", t.size,
               " side:", to_char(t.side), " | Top Bid: ", top.bid_price, "@", top.bid_size,
               " | Top Ask: ", top.ask_price, "@", top.ask_size);
}

class Pipeline
{
   public:
    Pipeline(std::ofstream& orders_out, std::ofstream& trades_out, IOrderBook& order_book,
             Strategy& strategy, IMatchingEngine& engine, Logger& logger)
        : orders_out_(orders_out),
          trades_out_(trades_out),
          order_book_(order_book),
          strategy_(strategy),
          engine_(engine),
          logger_(logger)
    {
        engine_.set_report_callback(
            [this](const ExecutionReport& report)
            {
                strategy_.on_order_update(report);
                if (report.exec_type == ExecType::Trade)
                {
                    if (report.owner == "MarketMaker")
                    {
                        ++trade_reports_;
                        trade_report_quantity_ += report.quantity;
                        fees_paid_ += report.fee;
                    }
                }
                write_trade_csv_row(trades_out_, report);
            });
    }

    void process_tick(const Tick& tick, bool enable_print = true)
    {
        order_book_.on_tick(tick);
        engine_.process_market_tick(tick);
        auto top = order_book_.market_top(tick.symbol);
        if (enable_print) print_tick(logger_, tick, top);

        submit_strategy_actions(tick.symbol, tick.ts, top);
    }

    void process_event(const MarketEvent& event, bool enable_print = false)
    {
        std::visit(
            [this, enable_print](const auto& value)
            {
                using Event = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Event, MarketTrade>)
                {
                    const auto quote_it = latest_quotes_.find(value.symbol);
                    strategy_.on_market_trade(
                        value, quote_it == latest_quotes_.end() ? nullptr : &quote_it->second);
                    engine_.process_market_tick({value.ts, value.symbol, value.price,
                                                 value.quantity, value.aggressor_side});
                }
                else
                {
                    latest_quotes_[value.symbol] = value;
                    order_book_.on_bbo(value);
                    engine_.process_bbo(value);
                    const auto top = order_book_.market_top(value.symbol);
                    if (enable_print)
                    {
                        logger_.log("[BBO] ", value.symbol, " ts:", value.ts,
                                    " bid:", value.bid_price, "@", value.bid_quantity,
                                    " ask:", value.ask_price, "@", value.ask_quantity);
                    }
                    submit_strategy_actions(value.symbol, value.ts, top);
                }
            },
            event);
    }

    void print_summary() const
    {
        const double fill_rate =
            metrics::compute_fill_rate(submitted_quantity_, trade_report_quantity_);
        const double cancel_rate =
            metrics::compute_cancel_rate(submitted_orders_, cancel_requests_);
        double exposure = metrics::compute_inventory_exposure(strategy_.net_position());
        const StrategyMetrics strategy_metrics = strategy_.metrics();

        logger_.log("[SUMMARY] submitted_orders=", submitted_orders_,
                    " submitted_quantity=", submitted_quantity_,
                    " cancel_requests=", cancel_requests_, " trade_reports=", trade_reports_,
                    " fill_rate=", fill_rate, " cancel_rate=", cancel_rate,
                    " trade_report_quantity=", trade_report_quantity_, " fees_paid=", fees_paid_,
                    " net_position=", strategy_.net_position(), " inventory_exposure=", exposure,
                    " working_orders=", strategy_.working_order_count());

        if (strategy_metrics.available)
        {
            const double strategy_fill_rate = metrics::compute_fill_rate(
                strategy_metrics.submitted_quantity, strategy_metrics.filled_quantity);
            logger_.log(
                "[STRATEGY_METRICS] submitted_quantity=", strategy_metrics.submitted_quantity,
                " filled_quantity=", strategy_metrics.filled_quantity,
                " fill_rate=", strategy_fill_rate, " fill_count=", strategy_metrics.fill_count,
                " cancel_count=", strategy_metrics.cancel_count,
                " quote_count=", strategy_metrics.quote_count,
                " fees_paid=", strategy_metrics.fees_paid,
                " captured_edge=", strategy_metrics.captured_edge,
                " adverse_selection=", strategy_metrics.adverse_selection,
                " markout_count=", strategy_metrics.markout_count,
                " total_quote_lifetime=", strategy_metrics.total_quote_lifetime,
                " max_quote_lifetime=", strategy_metrics.max_quote_lifetime,
                " avg_abs_inventory=", strategy_metrics.average_abs_inventory,
                " max_abs_inventory=", strategy_metrics.max_abs_inventory,
                " inventory_sign_changes=", strategy_metrics.inventory_sign_changes);
        }
    }

   private:
    void submit_strategy_actions(const std::string& symbol, uint64_t ts, const TopOfBook& top)
    {
        auto orders = strategy_.on_top_of_book(symbol, top);

        for (uint64_t order_id : strategy_.cancel_requests())
        {
            ++cancel_requests_;
            engine_.cancel_order(order_id);
        }

        for (auto& order : orders)
        {
            order.order_id = next_order_id_++;
            auto ex_order = to_exchange_order(order, ts, "MarketMaker");
            ++submitted_orders_;
            submitted_quantity_ += order.quantity;
            write_order_csv_row(orders_out_, ts, order);
            strategy_.on_order_submitted(order);
            engine_.send_order(ex_order);
        }
    }
    std::ofstream& orders_out_;
    std::ofstream& trades_out_;
    IOrderBook& order_book_;
    Strategy& strategy_;
    IMatchingEngine& engine_;
    Logger& logger_;
    std::unordered_map<std::string, BboQuote> latest_quotes_;
    std::atomic<uint64_t> next_order_id_{1};
    uint64_t submitted_orders_{0};
    uint64_t submitted_quantity_{0};
    uint64_t cancel_requests_{0};
    uint64_t trade_reports_{0};
    uint64_t trade_report_quantity_{0};
    double fees_paid_{0.0};
};

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
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine,
                      logger);
    CsvFeed feed(
        csv_file, [&](const Tick& tick) { pipeline.process_tick(tick, true); }, cfg.delay, &logger);
    feed.run();
    pipeline.print_summary();
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
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine,
                      logger);
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
    pipeline.print_summary();
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
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine,
                      logger);
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