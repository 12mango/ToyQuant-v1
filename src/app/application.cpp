#include "app/application.h"

#include <filesystem>
#include <immintrin.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "accounting/portfolio.h"
#include "app/pipeline.h"
#include "app/strategy_factory.h"
#include "common/instrument_spec.h"
#include "exchange/matching_engine.h"
#include "legacy/tick.h"
#include "legacy/tick_pipeline.h"
#include "market/csv_feed.h"
#include "market/l2_replay_feed.h"
#include "market/replay_feed.h"
#include "market/udp_feed.h"
#include "orderbook/orderbook.h"
#include "orderbook/l2_orderbook.h"
#include "utils/logger.h"

namespace
{
std::string application_log_path(bool discard_output)
{
    if (!discard_output) return to_abs_path("logs/toy_quant.log");
#ifdef _WIN32
    return "NUL";
#else
    return "/dev/null";
#endif
}
}  // namespace

Application::Application(const AppConfig& cfg) : cfg_(cfg) {}

void Application::configure_logger(Logger& logger) const
{
    if (cfg_.discard_output) logger.set_enabled(false);
}

Application::OutputFiles Application::open_output_files(const std::string& source,
                                                       const std::string& source_type) const
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

    const std::string output_target = cfg_.discard_output
#ifdef _WIN32
                                          ? "NUL"
#else
                                          ? "/dev/null"
#endif
                                          : orders_file;
    const std::string trades_target = cfg_.discard_output
#ifdef _WIN32
                                          ? "NUL"
#else
                                          ? "/dev/null"
#endif
                                          : trades_file;
    OutputFiles files{std::ofstream(output_target), std::ofstream(trades_target)};
    if (!files.orders.is_open() || !files.trades.is_open())
    {
        throw std::runtime_error("failed to open runtime output files in '" + output_dir.string() +
                                 "'");
    }
    if (!cfg_.discard_output)
    {
        files.orders << "# " << source_type << "=" << source << "\n";
        files.trades << "# " << source_type << "=" << source << "\n";
        files.orders << "ts,symbol,side,price,quantity,order_id\n";
        files.trades << "ts,symbol,side,price,quantity,order_id,liquidity_role,fee\n";
    }
    return files;
}

int Application::run() const
{
    switch (cfg_.mode)
    {
        case AppMode::LegacyCsv:
            run_legacy_csv_mode();
            return 0;
        case AppMode::LegacyUdp:
            run_legacy_udp_mode();
            return 0;
        case AppMode::Replay:
            run_replay_mode();
            return 0;
        case AppMode::L2Replay:
            run_l2_replay_mode();
            return 0;
    }
    return 1;
}

void Application::run_legacy_csv_mode() const
{
    const std::string csv_file = to_abs_path(cfg_.path_or_port);
    Logger logger(application_log_path(cfg_.discard_output));
    configure_logger(logger);
    logger.log("[Mode: Legacy CSV] Opening: ", csv_file, " (delay: ", cfg_.delay,
               "ms) strategy=", cfg_.strategy_name);

    auto output_files = open_output_files(csv_file);
    OrderBook order_book;
    auto strategy = make_strategy(cfg_.strategy_name);
    MatchingEngine engine;
    Portfolio portfolio;
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine,
                      portfolio);
    legacy::TickPipeline tick_pipeline(pipeline, order_book, order_book, engine);
    pipeline.set_collateral(1000.0, 0.0);
    pipeline.set_position_limit(cfg_.position_limit);
    CsvFeed feed(
        csv_file, [&](const legacy::Tick& tick) { tick_pipeline.process(tick); }, cfg_.delay,
        &logger);
    feed.run();
    logger.log(pipeline.summary().to_log_string());
}

void Application::run_replay_mode() const
{
    if (cfg_.symbol != "BTCUSDT")
        throw std::invalid_argument("no instrument specification for replay symbol: " + cfg_.symbol);

    const InstrumentSpec instrument = btc_usdt_spec(cfg_.quantity_scale);
    const std::string trades_file = to_abs_path(cfg_.path_or_port);
    const std::string quotes_file = to_abs_path(cfg_.quotes_path);
    Logger logger(application_log_path(cfg_.discard_output));
    configure_logger(logger);
    logger.log("[Mode: Replay] trades=", trades_file, " quotes=", quotes_file,
               " symbol=", cfg_.symbol, " quantity_scale=", cfg_.quantity_scale,
               " strategy=", cfg_.strategy_name);

    const std::string source = "binance;trades=" + trades_file + ";quotes=" + quotes_file +
                               ";symbol=" + cfg_.symbol +
                               ";tick_size=" + std::to_string(instrument.tick_size) +
                               ";quantity_scale=" + std::to_string(instrument.quantity_scale) +
                               ";maker_fee=" + std::to_string(instrument.maker_fee_rate) +
                               ";taker_fee=" + std::to_string(instrument.taker_fee_rate);

    auto output_files = open_output_files(source, "source_market_data");
    OrderBook order_book(instrument.tick_size);
    // The L1 tuning knobs keep their defaults here; only the sizing is settable from the command
    // line, because the instrument's quantity scale is not an economic scale for a contract.
    auto strategy = make_strategy(cfg_.strategy_name, &instrument, {}, 0.60, 2.5, 0.10, 0.40,
                                  cfg_.order_size_override, cfg_.inventory_limit_override,
                                  cfg_.base_spread_ticks);
    MatchingEngine engine(instrument.tick_size,
                          FeeSchedule{.maker_rate = instrument.maker_fee_rate,
                                      .taker_rate = instrument.taker_fee_rate,
                                      .quantity_scale = instrument.quantity_scale,
                                      .unit_notional_usd = instrument.unit_notional_usd},
                          1, cfg_.queue_model);
    Portfolio portfolio(instrument.quantity_scale, 1000.0, instrument.unit_notional_usd);
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine,
                      portfolio, instrument.tick_size);
    // The replay can end holding a position, so the reported equity has to include its mark to
    // market. Every other mode already does this; the L1 replay used to leave equity and
    // unrealized PnL at zero no matter what the position was.
    double final_mid = 0.0;
    ReplayFeed feed(
        make_market_data_readers("binance", trades_file, quotes_file, instrument),
        [&](const MarketEvent& event)
        {
            if (const auto* quote = std::get_if<BboQuote>(&event))
                final_mid = (quote->bid_price + quote->ask_price) / 2.0;
            pipeline.process_event(event);
        },
        cfg_.delay);
    feed.run();
    if (final_mid > 0.0) portfolio.mark_to_market({{cfg_.symbol, final_mid}});
    const auto& validation = feed.validation_summary();
    logger.log("[DATA] status=", validation.has_soft_issues() ? "warning" : "ok",
               " events=", validation.events, " trades=", validation.trades,
               " quotes=", validation.quotes, " trades_without_bbo=", validation.trades_without_bbo,
               " stale_trades=", validation.stale_trades,
               " dislocated_trades=", validation.dislocated_trades,
               " max_bbo_age_ms=", validation.max_bbo_age_ms,
               " max_trade_deviation_bps=", validation.max_trade_deviation_bps);
    logger.log(pipeline.summary().to_log_string());
}

void Application::run_l2_replay_mode() const
{
    if (cfg_.symbol != "BTC-PERPETUAL")
        throw std::invalid_argument("no instrument specification for L2 replay symbol: " +
                                    cfg_.symbol);

    const InstrumentSpec instrument = deribit_btc_perpetual_spec();
    const std::string trades_file = to_abs_path(cfg_.path_or_port);
    const std::string depth_file = to_abs_path(cfg_.quotes_path);
    Logger logger(application_log_path(cfg_.discard_output));
    configure_logger(logger);
    logger.log("[Mode: L2 Replay] trades=", trades_file, " depth=", depth_file,
               " symbol=", cfg_.symbol, " strategy=", cfg_.strategy_name,
               " queue_model=", queue_model_name(cfg_.queue_model));

    // Per-stage timing is opt-in. The profiler allocates nothing until sampling starts, so
    // a run without --profile-stages pays nothing beyond this construction.
    StageProfiler stage_profiler(cfg_.profile_sample_interval > 0
                                     ? cfg_.profile_sample_interval
                                     : StageProfiler::kDefaultSampleInterval);
    StageProfiler* const profiler = cfg_.profile_sample_interval > 0 ? &stage_profiler : nullptr;

    const std::string source = "deribit;trades=" + trades_file + ";depth=" + depth_file +
                               ";symbol=" + cfg_.symbol +
                               ";tick_size=" + std::to_string(instrument.tick_size);
    auto output_files = open_output_files(source, "source_l2_market_data");
    OrderBook execution_book(instrument.tick_size);
    L2OrderBook l2_book(instrument.tick_size);
    auto strategy = make_strategy(cfg_.strategy_name, &instrument, {}, 0.60, 2.5, 0.10, 0.40,
                                  cfg_.order_size_override, cfg_.inventory_limit_override,
                                  cfg_.base_spread_ticks);
    MatchingEngine engine(instrument.tick_size,
                          FeeSchedule{.maker_rate = instrument.maker_fee_rate,
                                      .taker_rate = instrument.taker_fee_rate,
                                      .quantity_scale = instrument.quantity_scale,
                                      .unit_notional_usd = instrument.unit_notional_usd},
                          1, cfg_.queue_model);
    Portfolio portfolio(instrument.quantity_scale, 1000.0, instrument.unit_notional_usd);
    Pipeline pipeline(output_files.orders, output_files.trades, execution_book, *strategy, engine,
                      portfolio, instrument.tick_size);
    // The documented model is cash only and no margin, so the cash the run starts with is the
    // exposure ceiling the summary measures against, and --max-position decides whether it is
    // enforced. 0, the default, reports the ceiling without blocking anything.
    pipeline.set_collateral(1000.0, instrument.unit_notional_usd);
    pipeline.set_position_limit(cfg_.position_limit);
    pipeline.set_profiler(profiler);
    double final_mid = 0.0;
    MarketDataValidationConfig validation_config;
    validation_config.validate_incremental_update_fields = !cfg_.fast_validation;
    L2ReplayFeed feed(
        make_deribit_trade_reader(trades_file),
        make_deribit_depth_reader(depth_file),
        [&](const MarketEvent& event)
        {
            std::visit(
                [&](const auto& value)
                {
                    using Event = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Event, MarketTrade>)
                        pipeline.process_l2_market_trade(value);
                    else if constexpr (std::is_same_v<Event, MarketDepthSnapshot>)
                    {
                        if (profiler != nullptr) profiler->begin(Stage::BookApply);
                        l2_book.apply_snapshot(value);
                        if (profiler != nullptr) profiler->end(Stage::BookApply);
                        if (profiler != nullptr) profiler->begin(Stage::MarketView);
                        const auto view = l2_book.market_view();
                        if (profiler != nullptr) profiler->end(Stage::MarketView);
                        final_mid = (view.top.bid_price + view.top.ask_price) / 2.0;
                        pipeline.process_l2_market_view(view);
                    }
                    else if constexpr (std::is_same_v<Event, IncrementalBookBatch>)
                    {
                        if (profiler != nullptr) profiler->begin(Stage::BookApply);
                        l2_book.apply_incremental_batch(value);
                        if (profiler != nullptr) profiler->end(Stage::BookApply);
                        if (profiler != nullptr) profiler->begin(Stage::MarketView);
                        const auto view = l2_book.market_view();
                        if (profiler != nullptr) profiler->end(Stage::MarketView);
                        final_mid = (view.top.bid_price + view.top.ask_price) / 2.0;
                        pipeline.process_l2_market_view(view);
                    }
                },
                event);
        },
        cfg_.delay, validation_config);
    feed.set_profiler(profiler);
    feed.run();
    if (final_mid > 0.0) portfolio.mark_to_market({{cfg_.symbol, final_mid}});
    const auto& validation = feed.validation_summary();
    logger.log("[L2 DATA] events=", validation.events, " trades=", validation.trades,
               " depth_snapshots=", validation.depth_snapshots,
               " incremental_batches=", validation.incremental_batches,
               " incremental_snapshot_batches=", validation.incremental_snapshot_batches);
    logger.log(pipeline.summary().to_log_string());
    if (profiler != nullptr)
    {
        // Written straight to stdout rather than through the logger, because --no-output
        // disables the logger and that is the recommended combination for profiling.
        std::cout << profiler->report() << std::flush;
    }
}

void Application::run_legacy_udp_mode() const
{
    int port = std::stoi(cfg_.path_or_port);
    Logger logger(application_log_path(cfg_.discard_output));
    configure_logger(logger);
    logger.log("[Mode: Legacy UDP] Listening on UDP port: ", port,
               "... strategy=", cfg_.strategy_name);

    auto output_files = open_output_files("udp://" + std::to_string(port));
    OrderBook order_book;
    auto strategy = make_strategy(cfg_.strategy_name);
    MatchingEngine engine;
    Portfolio portfolio;
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine,
                      portfolio);
    legacy::TickPipeline tick_pipeline(pipeline, order_book, order_book, engine);
    pipeline.set_collateral(1000.0, 0.0);
    pipeline.set_position_limit(cfg_.position_limit);
    UdpFeed feed(port);
    feed.start();

    legacy::Tick tick;
    while (true)
    {
        if (feed.pop_tick(tick))
        {
            tick_pipeline.process(tick);
        }
        else
        {
            _mm_pause();
        }
    }
}
