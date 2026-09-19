#include "app/application.h"

#include <filesystem>
#include <immintrin.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include "accounting/portfolio.h"
#include "app/pipeline.h"
#include "app/strategy_factory.h"
#include "common/instrument_spec.h"
#include "exchange/matching_engine.h"
#include "legacy/tick.h"
#include "legacy/tick_pipeline.h"
#include "market/csv_feed.h"
#include "market/replay_feed.h"
#include "market/udp_feed.h"
#include "orderbook/orderbook.h"
#include "utils/logger.h"

Application::Application(const AppConfig& cfg) : cfg_(cfg) {}

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
    }
    return 1;
}

void Application::run_legacy_csv_mode() const
{
    const std::string csv_file = to_abs_path(cfg_.path_or_port);
    Logger logger(to_abs_path("logs/toy_quant.log"));
    logger.log("[Mode: Legacy CSV] Opening: ", csv_file, " (delay: ", cfg_.delay,
               "ms) strategy=", cfg_.strategy_name);

    auto output_files = open_output_files(csv_file);
    OrderBook order_book;
    auto strategy = make_strategy(cfg_.strategy_name);
    MatchingEngine engine(&logger);
    Portfolio portfolio;
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine,
                      portfolio, logger);
    legacy::TickPipeline tick_pipeline(pipeline, order_book, order_book, engine);
    CsvFeed feed(
        csv_file, [&](const legacy::Tick& tick) { tick_pipeline.process(tick, true); }, cfg_.delay,
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
    Logger logger(to_abs_path("logs/toy_quant.log"));
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
    auto strategy = make_strategy(cfg_.strategy_name, &instrument);
    MatchingEngine engine(&logger, instrument.tick_size,
                          FeeSchedule{.maker_rate = instrument.maker_fee_rate,
                                      .taker_rate = instrument.taker_fee_rate,
                                      .quantity_scale = instrument.quantity_scale});
    Portfolio portfolio(instrument.quantity_scale);
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine,
                      portfolio, logger);
    ReplayFeed feed(
        make_market_data_readers("binance", trades_file, quotes_file, instrument),
        [&](const MarketEvent& event) { pipeline.process_event(event); }, cfg_.delay);
    feed.run();
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

void Application::run_legacy_udp_mode() const
{
    int port = std::stoi(cfg_.path_or_port);
    Logger logger(to_abs_path("logs/toy_quant.log"));
    logger.log("[Mode: Legacy UDP] Listening on UDP port: ", port,
               "... strategy=", cfg_.strategy_name);

    auto output_files = open_output_files("udp://" + std::to_string(port));
    OrderBook order_book;
    auto strategy = make_strategy(cfg_.strategy_name);
    MatchingEngine engine(&logger);
    Portfolio portfolio;
    Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine,
                      portfolio, logger);
    legacy::TickPipeline tick_pipeline(pipeline, order_book, order_book, engine);
    UdpFeed feed(port);
    feed.start();

    legacy::Tick tick;
    while (true)
    {
        if (feed.pop_tick(tick))
        {
            tick_pipeline.process(tick, true);
        }
        else
        {
            _mm_pause();
        }
    }
}
