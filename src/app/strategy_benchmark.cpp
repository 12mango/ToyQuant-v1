#include "app/strategy_benchmark.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#include "app/pipeline.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "accounting/portfolio.h"
#include "app/strategy_factory.h"
#include "common/instrument_spec.h"
#include "exchange/matching_engine.h"
#include "market/replay_feed.h"
#include "orderbook/orderbook.h"
#include "utils/logger.h"

namespace
{
struct StrategyBenchmarkEnv
{
    std::unique_ptr<Strategy> strategy;
    Portfolio portfolio;
    OrderBook order_book;
    std::ofstream orders;
    std::ofstream trades;
    std::unique_ptr<Logger> logger;
    std::vector<StrategyBenchmarkResult> results;
};
}  // namespace

std::vector<StrategyBenchmarkResult> run_strategy_benchmark(const std::string& trades_path,
                                                            const std::string& quotes_path,
                                                            const std::string& symbol,
                                                            uint64_t quantity_scale,
                                                            const FlowAwareMarketMakerConfig& flow_config,
                                                            double l1_risk_threshold,
                                                            double l1_stress_spread_multiplier,
                                                            double l1_minimum_stress_quantity_ratio,
                                                            double l1_fee_spread_multiplier,
                                                            const std::string& timeline_path)
{
    if (symbol.empty()) throw std::invalid_argument("benchmark symbol cannot be empty");

    const InstrumentSpec instrument = btc_usdt_spec(quantity_scale);
    const std::vector<std::string> names = {"passive_l1", "inventory_aware_l1",
                                            "flow_aware_l1", "active_l1", "l1"};
    std::vector<StrategyBenchmarkResult> results;
    results.reserve(names.size());
    std::ofstream timeline;
    if (!timeline_path.empty())
    {
        timeline.open(timeline_path);
        if (!timeline) throw std::runtime_error("failed to open benchmark timeline: " + timeline_path);
        timeline << "strategy,hour,ts,orders,fills,filled_qty,position,gross_pnl,net_pnl,fees,"
                    "filtered,edge,markout,avg_inv,max_inv,inv_flip\n";
    }

    for (const auto& name : names)
    {
        const std::string orders_path = "/tmp/strategy_benchmark_orders_" + name + ".csv";
        const std::string trades_out_path = "/tmp/strategy_benchmark_trades_" + name + ".csv";

        std::ofstream orders(orders_path);
        std::ofstream trades(trades_out_path);
        if (!orders || !trades)
            throw std::runtime_error("failed to open temporary benchmark files for strategy: " + name);

        orders << "ts,symbol,side,price,quantity,order_id\n";
        trades << "ts,symbol,side,price,quantity,order_id,liquidity_role,fee\n";

        auto strategy = make_strategy(name, &instrument, flow_config, l1_risk_threshold,
                          l1_stress_spread_multiplier,
                                      l1_minimum_stress_quantity_ratio,
                                      l1_fee_spread_multiplier);
        OrderBook order_book(instrument.tick_size);
        MatchingEngine engine(nullptr, instrument.tick_size,
                              FeeSchedule{.maker_rate = instrument.maker_fee_rate,
                                          .taker_rate = instrument.taker_fee_rate,
                                          .quantity_scale = instrument.quantity_scale},
                              1);
        Portfolio portfolio(instrument.quantity_scale);
        Logger logger;
        Pipeline pipeline(orders, trades, order_book, *strategy, engine, portfolio, logger,
                  instrument.tick_size);

        double final_mid = 0.0;
        uint64_t last_event_ts = 0;
        uint64_t replay_start_ts = 0;
        uint64_t next_timeline_ts = 0;
        uint64_t timeline_hour = 0;
        auto write_timeline = [&](uint64_t ts)
        {
            if (!timeline || replay_start_ts == 0) return;
            portfolio.mark_to_market({{symbol, final_mid}});
            const auto snapshot = pipeline.summary();
            const auto& quality = snapshot.execution_quality;
            const double gross = snapshot.portfolio.realized_pnl + snapshot.portfolio.fees_paid +
                                 snapshot.portfolio.unrealized_pnl;
            timeline << name << ',' << timeline_hour << ',' << ts << ','
                     << snapshot.submitted_orders << ',' << snapshot.trade_reports << ','
                     << snapshot.trade_report_quantity << ',' << strategy->net_position() << ','
                     << gross << ',' << snapshot.portfolio.equity - 1000.0 << ','
                     << snapshot.portfolio.fees_paid << ',' << snapshot.filtered_market_trades << ','
                     << quality.captured_edge << ',' << quality.adverse_selection << ','
                     << quality.average_abs_inventory << ',' << quality.max_abs_inventory << ','
                     << quality.inventory_sign_changes << '\n';
        };
        ReplayFeed feed(make_market_data_readers("binance", trades_path, quotes_path, instrument),
                       [&](const MarketEvent& event)
                       {
                           const uint64_t ts = std::visit(
                               [](const auto& value) { return value.ts; }, event);
                           last_event_ts = ts;
                           if (replay_start_ts == 0)
                           {
                               replay_start_ts = ts;
                               next_timeline_ts = ts + 60ULL * 60ULL * 1000ULL;
                           }
                           if (const auto* quote = std::get_if<BboQuote>(&event))
                           {
                               while (timeline && ts >= next_timeline_ts)
                               {
                                   write_timeline(next_timeline_ts);
                                   ++timeline_hour;
                                   next_timeline_ts += 60ULL * 60ULL * 1000ULL;
                               }
                               final_mid = (quote->bid_price + quote->ask_price) / 2.0;
                           }
                           pipeline.process_event(event);
                       },
                       0);
        feed.run();
        if (final_mid > 0.0) portfolio.mark_to_market({{symbol, final_mid}});
        if (timeline && replay_start_ts != 0)
        {
            ++timeline_hour;
            write_timeline(last_event_ts);
        }
        const auto summary = pipeline.summary();
        const auto& validation = feed.validation_summary();
        StrategyBenchmarkResult result;
        result.strategy_name = name;
        result.submitted_orders = summary.submitted_orders;
        result.submitted_quantity = summary.submitted_quantity;
        result.trade_reports = summary.trade_reports;
        result.filled_quantity = summary.trade_report_quantity;
        result.filtered_market_trades = summary.filtered_market_trades;
        result.cancel_requests = summary.cancel_requests;
        result.net_position = strategy->net_position();
        result.fill_rate = summary.fill_rate;
        result.cancel_rate = summary.cancel_rate;
        result.realized_pnl = summary.portfolio.realized_pnl;
        result.equity = summary.portfolio.equity;
        result.gross_pnl = summary.portfolio.realized_pnl + summary.portfolio.fees_paid +
                   summary.portfolio.unrealized_pnl;
        result.fees_paid = summary.portfolio.fees_paid;
        result.fee_ratio = std::abs(result.gross_pnl) > 1e-12
                       ? result.fees_paid / std::abs(result.gross_pnl)
                       : 0.0;
        result.maker_trade_count = summary.portfolio.maker_trade_count;
        result.taker_trade_count = summary.portfolio.taker_trade_count;
        const auto& quality = summary.execution_quality;
        result.captured_edge = quality.captured_edge;
        result.adverse_selection = quality.adverse_selection;
        result.average_abs_inventory = quality.average_abs_inventory;
        result.max_abs_inventory = quality.max_abs_inventory;
        result.inventory_sign_changes = quality.inventory_sign_changes;
        result.markout_count = quality.markout_count;
        result.total_quote_lifetime = quality.total_quote_lifetime;
        result.max_quote_lifetime = quality.max_quote_lifetime;
        result.quote_observations = quality.quote_observations;
        result.bbo_quote_observations = quality.bbo_quote_observations;
        result.total_quote_distance = quality.total_quote_distance;
        result.audited_cancelled_orders = quality.audited_cancelled_orders;
        result.cancelled_before_fill_orders = quality.cancelled_before_fill_orders;
        result.total_order_lifetime_cycles = quality.total_order_lifetime_cycles;
        result.working_orders = summary.working_orders;
        result.stale_trades = validation.stale_trades;
        result.dislocated_trades = validation.dislocated_trades;
        result.max_trade_deviation_bps = validation.max_trade_deviation_bps;
        results.push_back(result);
    }

    return results;
}
