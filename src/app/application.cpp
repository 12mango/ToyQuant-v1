#include "app/application.h"

#include <filesystem>
#include <immintrin.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "lines/carry_ledger.h"
#include "accounting/portfolio.h"
#include "lines/reconciliation.h"
#include "app/pipeline.h"
#include "app/strategy_factory.h"
#include "common/instrument_spec.h"
#include "exchange/matching_engine.h"
#include "lines/order_gateway.h"
#include "lines/paper_broker.h"
#include "common/tick.h"
#include "lines/legacy/tick_pipeline.h"
#include "lines/legacy/csv_feed.h"
#include "market/funding_rate.h"
#include "market/l2_replay_feed.h"
#include "market/market_event_recorder.h"
#include "market/threaded_l2_replay.h"
#include "market/replay_feed.h"
#include "lines/legacy/udp_feed.h"
#include "market/ws_feed.h"
#include "lines/legacy/order_book.h"
#include "orderbook/l2_orderbook.h"
#include "utils/logger.h"

namespace
{
// The schedule name app_config accepted, as the strategy's own enum. app_config is the one that validates the
// three names, so an unrecognised string cannot reach here.
ExecutionAlgo execution_algo_of(const std::string& name)
{
    if (name == "twap") return ExecutionAlgo::Twap;
    if (name == "pov") return ExecutionAlgo::Pov;
    return ExecutionAlgo::Market;
}

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

MatchingEngineConfig Application::engine_config(const InstrumentSpec& instrument) const
{
    // `--queue-seed=0` means "use the default seed", which is why that decision lives here rather than in the
    // parser: the flag's zero is this mapping's business, and there is one mapping for both modes that build
    // an engine from these flags.
    //
    // `cancel_delay_events = 1` is the one field here that no flag sets. It is the original event-counted
    // cancel delay that `cancel_latency_us` was added underneath, and every recorded run has carried it; it
    // used to be a bare `1` as the third argument of the engine's parameter list, which is a value nobody
    // could read. Moving these into a struct without it is what the gate caught: every L2 and L1 number moved
    // the moment the one-event delay was dropped.
    return MatchingEngineConfig{
        .cancel_delay_events = 1,
        .queue_model = cfg_.queue_model,
        .queue_lump_chunks = cfg_.queue_lump_chunks,
        .queue_seed = cfg_.queue_seed == 0 ? kDefaultQueueSeed : cfg_.queue_seed,
        .arrival_share = cfg_.arrival_share,
        .order_latency_us = cfg_.order_latency_us,
        .cancel_latency_us = cfg_.cancel_latency_us,
        .lot_size = instrument.lot_size,
        .min_order_quantity = instrument.min_order_quantity,
        .participation_limit = cfg_.participation_limit,
    };
}

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

Logger Application::make_logger() const
{
    Logger logger(application_log_path(cfg_.discard_output));
    configure_logger(logger);
    return logger;
}

void Application::apply_pipeline_limits(Pipeline& pipeline) const
{
    pipeline.set_position_limit(cfg_.position_limit);
    pipeline.set_hard_exposure_limit_usd(cfg_.max_exposure_usd);
    pipeline.set_kill_switch_loss_usd(cfg_.kill_switch_loss_usd);
    pipeline.set_fat_finger_bps(cfg_.fat_finger_bps);
    pipeline.set_order_rate_limit(cfg_.max_orders_per_second, cfg_.order_burst);
    pipeline.set_time_in_force(cfg_.time_in_force, cfg_.time_in_force_set);
    pipeline.set_trace_order(cfg_.trace_order);
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
        case AppMode::WebSocket:
            // One delivery path for both: the live session is the replay with its readers swapped for
            // the socket, so the two modes differ only in where the market events come from.
            run_l2_session_mode();
            return 0;
        case AppMode::Carry:
            run_carry_mode();
            return 0;
    }
    return 1;
}

void Application::run_carry_mode() const
{
    // The venue's fee schedule comes from the instrument specification, looked up by symbol, so a carry
    // cannot be priced with a rate the command line invented.
    InstrumentSpec venue_a;
    InstrumentSpec venue_b;
    if (!instrument_spec_of(cfg_.symbol, venue_a))
        throw std::invalid_argument("no instrument specification for carry symbol: " + cfg_.symbol);
    if (!instrument_spec_of(cfg_.symbol_b, venue_b))
        throw std::invalid_argument("no instrument specification for carry symbol: " + cfg_.symbol_b);

    Logger logger = make_logger();
    logger.log("[Mode: Carry] leg_a=", cfg_.symbol, " ", to_abs_path(cfg_.path_or_port),
               " leg_b=", cfg_.symbol_b, " ", to_abs_path(cfg_.funding_b_path),
               " notional_usd=", cfg_.carry_notional_usd,
               " long=", cfg_.carry_leg_a_long ? "a" : "b");

    CarryConfig config;
    config.leg_a = CarryLeg{
        .instrument = venue_a,
        .rates = read_funding_rate_series(to_abs_path(cfg_.path_or_port), cfg_.symbol),
        .long_side = cfg_.carry_leg_a_long};
    config.leg_b = CarryLeg{
        .instrument = venue_b,
        .rates = read_funding_rate_series(to_abs_path(cfg_.funding_b_path), cfg_.symbol_b),
        .long_side = !cfg_.carry_leg_a_long};
    config.notional_usd = cfg_.carry_notional_usd;
    // No orders and no trades file: the position is stated rather than matched against a book, and the
    // report is the whole output.
    logger.log(run_carry(std::move(config)).to_log_string());
}

void Application::run_legacy_csv_mode() const
{
    const std::string csv_file = to_abs_path(cfg_.path_or_port);
    Logger logger = make_logger();
    logger.log("[Mode: Legacy CSV] Opening: ", csv_file, " (delay: ", cfg_.delay,
               "ms) strategy=", cfg_.strategy_name);

    auto output_files = open_output_files(csv_file);
    OrderBook order_book;
    auto strategy = make_strategy(cfg_.strategy_name);
    MatchingEngine engine;
    Portfolio portfolio;
    Pipeline pipeline(output_files.orders, output_files.trades, *strategy, engine, portfolio);
    legacy::TickPipeline tick_pipeline(pipeline, order_book, order_book, engine);
    pipeline.set_collateral(1000.0, 0.0);
    apply_pipeline_limits(pipeline);
    CsvFeed feed(
        csv_file, [&](const legacy::Tick& tick) { tick_pipeline.process(tick); }, cfg_.delay,
        &logger);
    feed.run();
    logger.log(pipeline.summary().to_log_string());
}

void Application::run_replay_mode() const
{
    // The specification comes from the symbol rather than from a fixed BTCUSDT default, because this path now
    // carries three symbols of one venue whose ticks differ by a factor of ten thousand.
    InstrumentSpec instrument;
    if (!instrument_spec_of(cfg_.symbol, instrument))
        throw std::invalid_argument("no instrument specification for replay symbol: " + cfg_.symbol);
    // The positional scale overrides the venue's own lot step; without one, the step is what makes a unit mean
    // the same thing on every symbol.
    if (cfg_.has_quantity_scale) instrument.quantity_scale = cfg_.quantity_scale;
    // The venue's fee schedule unless a run asks for a different one. The override exists for the fee
    // question, so it is echoed in the mode line rather than left for a reader to infer from the numbers.
    if (cfg_.has_maker_fee_rate) instrument.maker_fee_rate = cfg_.maker_fee_rate;
    const std::string trades_file = to_abs_path(cfg_.path_or_port);
    const std::string quotes_file = to_abs_path(cfg_.quotes_path);
    Logger logger = make_logger();
    logger.log("[Mode: Replay] trades=", trades_file, " quotes=", quotes_file,
               " symbol=", cfg_.symbol, " quantity_scale=", instrument.quantity_scale,
               " strategy=", cfg_.strategy_name, " maker_fee_rate=", instrument.maker_fee_rate);

    const std::string source = "binance;trades=" + trades_file + ";quotes=" + quotes_file +
                               ";symbol=" + cfg_.symbol +
                               ";tick_size=" + std::to_string(instrument.tick_size) +
                               ";quantity_scale=" + std::to_string(instrument.quantity_scale) +
                               ";maker_fee=" + std::to_string(instrument.maker_fee_rate) +
                               ";taker_fee=" + std::to_string(instrument.taker_fee_rate);

    auto output_files = open_output_files(source, "source_market_data");
    // The L1 tuning knobs keep their defaults here; only the sizing is settable from the command
    // line, because the instrument's quantity scale is not an economic scale for a contract.
    auto strategy = make_strategy(cfg_.strategy_name,
                                  StrategyFactoryConfig{
                                      .instrument = &instrument,
                                      .order_size_override = cfg_.order_size_override,
                                      .inventory_limit_override = cfg_.inventory_limit_override,
                                      .base_spread_ticks_override = cfg_.base_spread_ticks,
                                      .inventory_shift_ticks_override = cfg_.inventory_shift_ticks,
                                      .refresh_price_ticks_override =
                                          cfg_.refresh_price_ticks_override,
                                      .edge_cover_ticks_override = cfg_.edge_cover_ticks,
                                      .min_spread_ticks_override = cfg_.min_spread_ticks,
                                      .trend = cfg_.trend,
                                      .execution = StrategyFactoryConfig::ExecutionParams{
                                          .side = cfg_.parent_side,
                                          .quantity = cfg_.parent_quantity,
                                          // This path's clock is milliseconds, so the deadline and the post
                                          // timeout are converted here rather than in the strategy.
                                          .horizon_us = cfg_.parent_horizon_us / 1'000,
                                          .algo = execution_algo_of(cfg_.algo),
                                          .slices = cfg_.algo_slices,
                                          .participation = cfg_.algo_participation,
                                          .post_first = cfg_.algo_entry == "post",
                                          .post_timeout_us = cfg_.algo_post_timeout_seconds * 1'000}});
    MatchingEngine engine(instrument.tick_size, fee_schedule_of(instrument),
                          engine_config(instrument));
    Portfolio portfolio(instrument.quantity_scale, 1000.0, instrument.unit_notional_usd,
                        instrument.multiplier);
    Pipeline pipeline(output_files.orders, output_files.trades, *strategy, engine, portfolio,
                      instrument.tick_size);
    // The replay path reports the same collateral and quantity scale as the L2 path, so its exposure
    // and edge figures are in dollars rather than in raw instrument units.
    pipeline.set_collateral(1000.0, instrument.unit_notional_usd, instrument.quantity_scale,
                            instrument.multiplier);
    pipeline.set_metrics(cfg_.metrics);
    pipeline.set_markout_horizon(cfg_.markout_horizon);
    // An execution run also works on this path: the touch is all a crossing child needs, and the parent's
    // clock lives in the one place both paths advance the midpoint. The L1 readers keep the venue's
    // millisecond event time while the L2 path carries microseconds, so both the strategy's deadline and the
    // measurement's are converted into this path's own clock here, and `ticks_per_second` tells the report
    // which unit it got.
    if (cfg_.has_parent)
        pipeline.set_parent_order(cfg_.parent_side, cfg_.parent_quantity,
                                  cfg_.parent_horizon_us / 1'000, 1'000);
    apply_pipeline_limits(pipeline);
    // The funding series is optional and owned here, because the pipeline only borrows it. A file that names
    // no settlement for this instrument is an error rather than an empty input, since a wrong symbol would
    // otherwise look like an instrument that never paid funding.
    FundingRateSeries funding_series;
    if (!cfg_.funding_rates_path.empty())
    {
        funding_series = read_funding_rate_series(to_abs_path(cfg_.funding_rates_path), instrument.symbol);
        if (funding_series.empty())
            throw std::invalid_argument("no funding settlements for " + instrument.symbol + " in " +
                                        cfg_.funding_rates_path);
        pipeline.set_funding_rates(&funding_series);
    }
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

void Application::run_l2_session_mode() const
{
    // A live session and a file replay differ only in where their readers come from, so one method
    // owns both: `live` selects the websocket for the market events, and every choice below that is
    // not about the source is shared. The file path needs its two inputs up front; the live one opens
    // its socket further down, beside the file readers it stands in for.
    const bool live = cfg_.mode == AppMode::WebSocket;
    std::string trades_file;
    std::string depth_file;
    std::string format;
    bool okx = false;
    if (!live)
    {
        trades_file = to_abs_path(cfg_.path_or_port);
        depth_file = to_abs_path(cfg_.quotes_path);
        // The depth file says which venue it came from, so the format is not a command line argument.
        format = market_data_format_of(depth_file);
        okx = format == "okx";
    }

    // The symbol decides the instrument, through the one registry the rest of the program reads, so a new
    // market is an entry there rather than another branch here. A symbol with no entry is an error rather
    // than a default, because a default would quietly price the wrong tick with the wrong fee.
    InstrumentSpec instrument;
    if (!instrument_spec_of(cfg_.symbol, instrument))
        throw std::invalid_argument("no instrument specification for L2 replay symbol: " + cfg_.symbol);
    // The positional quantity scale is accepted here for the same command shape as the other modes and then not
    // applied: on this path the instrument's own scale is the contract's, and every recorded run passes a scale of
    // one against a specification that carries a million, so honouring it would reprice the whole corpus. The mode
    // line prints the figure actually in force, which is what keeps the no-op visible rather than silent.
    // A different maker fee rate than the venue's, for the runs that ask what the fee would have had to be.
    if (cfg_.has_maker_fee_rate) instrument.maker_fee_rate = cfg_.maker_fee_rate;
    // And the account's own schedule, when a run names one. It is applied here, before the instrument reaches
    // the venue, the strategy and the pipeline, so all three price the schedule the mode line reports. The five
    // terms are the ones `InstrumentSpec` and `--fee-schedule` have in common, which is why they are listed one
    // by one: a term added to the flag's struct and not to the instrument prices the run without it while the
    // mode line reports it set, the failure `fee_schedule_of` exists to prevent in the other direction.
    if (cfg_.has_fee_schedule)
    {
        instrument.min_commission_per_order = cfg_.fee_schedule.min_commission_per_order;
        instrument.regulatory_fee_rate = cfg_.fee_schedule.regulatory_fee_rate;
        instrument.clearing_fee_per_share = cfg_.fee_schedule.clearing_fee_per_share;
        instrument.audit_fee_per_share = cfg_.fee_schedule.audit_fee_per_share;
        instrument.maker_rebate_per_share = cfg_.fee_schedule.maker_rebate_per_share;
    }
    Logger logger = make_logger();
    if (live)
        logger.log("[Mode: WebSocket] endpoint=", cfg_.ws_host, ":", cfg_.ws_port,
                   " target=", cfg_.ws_target, " symbol=", cfg_.symbol,
                   " strategy=", cfg_.strategy_name,
                   " queue_model=", queue_model_name(cfg_.queue_model),
                   " maker_fee_rate=", instrument.maker_fee_rate,
                   " min_commission_per_order=", instrument.min_commission_per_order,
                   " regulatory_fee_rate=", instrument.regulatory_fee_rate,
                   " clearing_fee_per_share=", instrument.clearing_fee_per_share,
                   " audit_fee_per_share=", instrument.audit_fee_per_share,
                   " maker_rebate_per_share=", instrument.maker_rebate_per_share);
    else
        logger.log("[Mode: L2 Replay] trades=", trades_file, " depth=", depth_file,
                   " symbol=", cfg_.symbol, " strategy=", cfg_.strategy_name,
                   " queue_model=", queue_model_name(cfg_.queue_model),
                   " maker_fee_rate=", instrument.maker_fee_rate,
                   " min_commission_per_order=", instrument.min_commission_per_order,
                   " regulatory_fee_rate=", instrument.regulatory_fee_rate,
                   " clearing_fee_per_share=", instrument.clearing_fee_per_share,
                   " audit_fee_per_share=", instrument.audit_fee_per_share,
                   " maker_rebate_per_share=", instrument.maker_rebate_per_share);

    // Per-stage timing is opt-in. The profiler allocates nothing until sampling starts, so
    // a run without --profile-stages pays nothing beyond this construction.
    StageProfiler stage_profiler(cfg_.profile_sample_interval > 0
                                     ? cfg_.profile_sample_interval
                                     : StageProfiler::kDefaultSampleInterval);
    StageProfiler* const profiler = cfg_.profile_sample_interval > 0 ? &stage_profiler : nullptr;
    // The profiler accumulates samples on whichever thread calls it, and the threaded path calls it
    // from two: the read and parse stages belong to the feed thread and the strategy stages to the
    // consumer. Sharing it would make every number in its report a race, so the combination is
    // refused rather than reported.
    if (cfg_.feed_thread && profiler != nullptr)
        throw std::invalid_argument(
            "--profile-stages and --feed-thread cannot be combined: the profiler accumulates on the "
            "thread that calls it and this path calls it from two, so its report would be a race "
            "rather than a measurement");

    // The source string names where the runtime files came from, so a live session records its
    // endpoint where a replay records its two files.
    const std::string source =
        live ? std::string("websocket;endpoint=") + cfg_.ws_host + ":" +
                   std::to_string(cfg_.ws_port) + ";symbol=" + cfg_.symbol +
                   ";tick_size=" + std::to_string(instrument.tick_size)
             : format + ";trades=" + trades_file + ";depth=" + depth_file + ";symbol=" + cfg_.symbol +
                   ";tick_size=" + std::to_string(instrument.tick_size);
    auto output_files = open_output_files(source, "source_l2_market_data");
    L2OrderBook l2_book(instrument.tick_size);
    auto strategy = make_strategy(cfg_.strategy_name,
                                  StrategyFactoryConfig{
                                      .instrument = &instrument,
                                      .order_size_override = cfg_.order_size_override,
                                      .inventory_limit_override = cfg_.inventory_limit_override,
                                      .base_spread_ticks_override = cfg_.base_spread_ticks,
                                      .inventory_shift_ticks_override = cfg_.inventory_shift_ticks,
                                      .refresh_price_ticks_override =
                                          cfg_.refresh_price_ticks_override,
                                      .edge_cover_ticks_override = cfg_.edge_cover_ticks,
                                      .markout_cover = cfg_.markout_cover,
                                      .flatten_band_units = cfg_.flatten_band,
                                      .min_spread_ticks_override = cfg_.min_spread_ticks,
                                      .trend = cfg_.trend,
                                      .execution = StrategyFactoryConfig::ExecutionParams{
                                          .side = cfg_.parent_side,
                                          .quantity = cfg_.parent_quantity,
                                          .horizon_us = cfg_.parent_horizon_us,
                                          .algo = execution_algo_of(cfg_.algo),
                                          .slices = cfg_.algo_slices,
                                          .participation = cfg_.algo_participation,
                                          .post_first = cfg_.algo_entry == "post",
                                          .post_timeout_us = cfg_.algo_post_timeout_seconds * 1'000'000}});
    MatchingEngine engine(instrument.tick_size, fee_schedule_of(instrument),
                          engine_config(instrument));
    // The paper broker wraps the venue rather than replacing it: the strategy, the book and the
    // accounting do not know it is there, and with no faults configured it forwards every call and every
    // report, which is what makes a run with it comparable to one without.
    std::unique_ptr<PaperBroker> broker;
    if (cfg_.paper_broker)
    {
        broker = std::make_unique<PaperBroker>(
            engine, BrokerFaults{.reject_every = cfg_.broker_reject_every,
                                 .partial_every = cfg_.broker_partial_every,
                                 .drop_every = cfg_.broker_drop_every,
                                 .outage_after = cfg_.broker_outage_after,
                                 .outage_holds = cfg_.broker_outage_holds});
    }
    IMatchingEngine* venue =
        broker ? static_cast<IMatchingEngine*>(broker.get()) : static_cast<IMatchingEngine*>(&engine);
    // The gateway is the client's side of the order path, so it sits outside the broker: what it absorbs,
    // times out or re-sends is decided before the broker or the venue sees anything. With every knob at
    // zero it forwards every call and every report, and the run is the run without it.
    std::unique_ptr<OrderGateway> gateway;
    if (cfg_.order_gateway)
    {
        gateway = std::make_unique<OrderGateway>(
            *venue, GatewayConfig{.ack_timeout_us = cfg_.gateway_ack_timeout_us,
                                  .retry_on_timeout = cfg_.gateway_retry_on_timeout});
        venue = gateway.get();
    }
    Portfolio portfolio(instrument.quantity_scale, 1000.0, instrument.unit_notional_usd,
                        instrument.multiplier);
    Pipeline pipeline(output_files.orders, output_files.trades, *strategy, *venue, portfolio,
                      instrument.tick_size);
    // The documented model is cash only and no margin, so the cash the run starts with is the
    // exposure ceiling the summary measures against, and --max-position decides whether it is
    // enforced. 0, the default, reports the ceiling without blocking anything.
    pipeline.set_collateral(1000.0, instrument.unit_notional_usd, instrument.quantity_scale,
                            instrument.multiplier);
    pipeline.set_profiler(profiler);
    apply_pipeline_limits(pipeline);
    // The broker's own commission and its floor, which are the only schedule terms the fee model needs at order
    // level rather than per fill: the floor is charged when an order ends, because that is when the order's own
    // total is known.
    pipeline.set_schedule_terms(instrument.fee_per_share, instrument.min_commission_per_order);
    // The adverse-selection gate needs the pipeline to measure what the venue's own prints were worth to a
    // passive fill, and it is the only way the strategy can know it: it never sees the midpoint after a fill.
    pipeline.set_markout_cover(cfg_.markout_cover);
    // The [GATE] line is printed when the run asked for either decision-layer diagnostic, so a run that asks
    // for neither keeps the summary it had.
    pipeline.set_gate_diagnostics(cfg_.markout_cover || cfg_.flatten_band > 0);
    // The equity series behind [PERFORMANCE], sampled where every path already advances the midpoint.
    // Off unless --metrics named it, so a run that does not ask prints the summary it always printed.
    pipeline.set_metrics(cfg_.metrics);
    // Where the round trip between a quote and its cost is measured, for the runs that asked for a horizon
    // other than the default.
    pipeline.set_markout_horizon(cfg_.markout_horizon);
    // The parent order is measured by the pipeline, not read back from the strategy: the [EXECUTION_ALGO]
    // line has to describe the order the run was given, whatever the schedule did with it.
    if (cfg_.has_parent)
        pipeline.set_parent_order(cfg_.parent_side, cfg_.parent_quantity, cfg_.parent_horizon_us);
    // The funding history of the instrument being replayed, applied as the event clock passes each
    // settlement. Optional, and owned here because the pipeline only borrows it.
    FundingRateSeries funding_series;
    if (!cfg_.funding_rates_path.empty())
    {
        funding_series = read_funding_rate_series(to_abs_path(cfg_.funding_rates_path), instrument.symbol);
        if (funding_series.empty())
            throw std::invalid_argument("no funding settlements for " + instrument.symbol + " in " +
                                        cfg_.funding_rates_path);
        pipeline.set_funding_rates(&funding_series);
    }
    double final_mid = 0.0;
    MarketDataValidationConfig validation_config;
    validation_config.validate_incremental_update_fields = !cfg_.fast_validation;
    // A venue brings either its own trade archive and a JSON Lines book, or the CSV pair this repository
    // started with, so the two readers are chosen together from the format the depth file reported. A
    // live session brings neither: it has one reader over one socket, and the feed that owns that socket
    // outlives it because it is declared above the L2 feed built below.
    std::unique_ptr<WebSocketFeed> live_feed;
    std::unique_ptr<IMarketEventReader> live_events;
    std::unique_ptr<IMarketEventReader> trades_reader;
    std::unique_ptr<IMarketEventReader> depth_reader;
    if (live)
    {
        live_feed = std::make_unique<WebSocketFeed>(
            WebSocketFeed::Endpoint{cfg_.ws_host, cfg_.ws_port, cfg_.ws_target},
            WebSocketFeed::Options{
                .ring_capacity = cfg_.ws_ring_capacity == 0 ? WebSocketFeed::kLiveRingCapacity
                                                           : cfg_.ws_ring_capacity,
                .live_seconds = cfg_.ws_seconds,
                .idle_wait_us = cfg_.idle_wait_us});
        live_feed->start();
        live_events = live_feed->event_reader();
    }
    else
    {
        trades_reader = okx ? make_okx_trade_reader(trades_file, instrument.symbol,
                                                    instrument.contract_size,
                                                    instrument.quantity_scale)
                            : make_deribit_trade_reader(trades_file);
        depth_reader = okx ? make_okx_book_reader(depth_file, instrument.symbol,
                                                  instrument.contract_size,
                                                  instrument.quantity_scale)
                           : make_deribit_depth_reader(depth_file);
    }
    // The fifth positional argument of l2_replay has always been a delay in milliseconds, applied as a fixed
    // sleep after every event. That neither reproduces the file's own timing nor scales, so a non-zero value now
    // means "pace this replay" and the speed stays at real time, while --live-pacing is the explicit form and
    // accepts a speed. Zero, which every recorded run passes, still reads the file as fast as it can.
    // A live session is paced by the venue, not by a schedule read from a file, so pacing is off there
    // whatever the flags say: there is no file clock to pace against.
    const ReplayPacing pacing{.enabled = !live && (cfg_.live_pacing || cfg_.delay > 0),
                              .speed = cfg_.live_pacing ? cfg_.live_pacing_speed : 1.0};
    // A recording of the delivered stream, when the run asked for one. It is owned here rather than
    // by the callback because its lifetime is the run's: the two files are opened once, and what
    // they hold is what the feed delivered.
    std::unique_ptr<MarketEventRecorder> recorder;
    if (!cfg_.record_prefix.empty())
        recorder = std::make_unique<MarketEventRecorder>(to_abs_path(cfg_.record_prefix));
    // The one consumer both delivery paths use. It runs on the calling thread either way, which is
    // what makes the threaded path comparable with the direct one rather than merely similar.
    const auto consume = [&](const MarketEvent& event)
    {
            // The tap sits before the pipeline, so the file is what the feed delivered rather than
            // what the strategy did with it, and a run that dies inside the strategy still holds
            // everything that arrived before the failure.
            if (recorder) recorder->record(event);
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
    };

    // Either the direct path or the thread-and-ring one, built from the same readers and the same
    // consumer, so the only thing a reader should be able to tell apart is the [LATENCY] line.
    std::unique_ptr<L2ReplayFeed> feed;
    std::unique_ptr<ThreadedReplayFeed> threaded_feed;
    const std::size_t hand_off_capacity =
        cfg_.ring_capacity == 0 ? ThreadedReplayFeed::kDefaultRingCapacity : cfg_.ring_capacity;
    if (cfg_.feed_thread)
    {
        if (live)
            threaded_feed = std::make_unique<ThreadedReplayFeed>(
                std::move(live_events), consume, pacing, validation_config, hand_off_capacity);
        else
            threaded_feed = std::make_unique<ThreadedReplayFeed>(
                std::move(trades_reader), std::move(depth_reader), consume, pacing, validation_config,
                hand_off_capacity);
    }
    else if (live)
    {
        feed = std::make_unique<L2ReplayFeed>(std::move(live_events), consume, pacing,
                                              validation_config);
    }
    else
    {
        feed = std::make_unique<L2ReplayFeed>(std::move(trades_reader), std::move(depth_reader),
                                              consume, pacing, validation_config);
    }
    if (threaded_feed)
    {
        threaded_feed->set_profiler(profiler);
        threaded_feed->run();
    }
    else
    {
        feed->set_profiler(profiler);
        feed->run();
    }
    const PacingSummary& pacing_summary =
        threaded_feed ? threaded_feed->pacing_summary() : feed->pacing_summary();
    const MarketDataValidationSummary& validation =
        threaded_feed ? threaded_feed->validation_summary() : feed->validation_summary();
    if (pacing.enabled) logger.log("[LIVE] ", pacing_summary.to_log_string());
    if (threaded_feed)
        logger.log("[LATENCY] ", threaded_feed->latency_summary().to_log_string());
    if (recorder)
    {
        // Flushed before the line is logged, because the line's claim is about the files on disk.
        recorder->flush();
        logger.log("[REC] ", recorder->summary().to_log_string(),
                   " trades_path=", recorder->trades_path(), " depth_path=", recorder->depth_path());
    }
    if (final_mid > 0.0) portfolio.mark_to_market({{cfg_.symbol, final_mid}});
    logger.log("[L2 DATA] events=", validation.events, " trades=", validation.trades,
               " depth_snapshots=", validation.depth_snapshots,
               " incremental_batches=", validation.incremental_batches,
               " incremental_snapshot_batches=", validation.incremental_snapshot_batches);
    logger.log(pipeline.summary().to_log_string());
    if (gateway != nullptr)
    {
        // The client's own account of the order path: what it sent, what it absorbed as a retry, and
        // what it never heard back about. Logged before the broker's line because it is the earlier
        // layer, and a timeout here is the reason the broker below may see a second copy.
        logger.log("[GATEWAY] ", gateway->summary().to_log_string());
    }
    if (broker != nullptr)
    {
        // The broker's own counts, then the two accounts compared: the statement is built from the
        // reports the venue sent and the portfolio from the reports the strategy received, so a fill the
        // strategy never heard about appears here as a disagreement between them.
        logger.log("[BROKER] ", broker->summary().to_log_string());
        logger.log("[RECON] ", reconcile_positions(broker->statement(), portfolio).to_log_string());
    }
    if (profiler != nullptr)
    {
        // Written straight to stdout rather than through the logger, because --no-output
        // disables the logger and that is the recommended combination for profiling.
        std::cout << profiler->report() << std::flush;
    }
    if (live_feed != nullptr)
    {
        // The socket is closed and its receive thread joined before this line is logged, so the
        // counters are the session's final ones rather than a race with a reader still draining.
        live_feed->stop();
        logger.log("[WS] ", live_feed->summary().to_log_string());
    }
}

void Application::run_legacy_udp_mode() const
{
    int port = std::stoi(cfg_.path_or_port);
    Logger logger = make_logger();
    logger.log("[Mode: Legacy UDP] Listening on UDP port: ", port,
               "... strategy=", cfg_.strategy_name);

    auto output_files = open_output_files("udp://" + std::to_string(port));
    OrderBook order_book;
    auto strategy = make_strategy(cfg_.strategy_name);
    MatchingEngine engine;
    Portfolio portfolio;
    Pipeline pipeline(output_files.orders, output_files.trades, *strategy, engine, portfolio);
    legacy::TickPipeline tick_pipeline(pipeline, order_book, order_book, engine);
    pipeline.set_collateral(1000.0, 0.0);
    apply_pipeline_limits(pipeline);
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
