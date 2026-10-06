#include "app/strategy_benchmark.h"

#include <filesystem>
#include <fstream>

#include "app/pipeline.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "accounting/portfolio.h"
#include "app/strategy_factory.h"
#include "common/instrument_spec.h"
#include "exchange/matching_engine.h"
#include "market/replay_feed.h"
#include "common/market_event.h"

namespace
{
// The two files a strategy's run writes are its audit trail, and nothing reads them: the numbers the caller
// gets come from the summary. They are written because a `Pipeline` writes one, so the benchmark measures the
// work a replay does -- which is also why they are not redirected to a null device, whose absence would
// change the timing this tool exists to report.
//
// What was wrong was where. The paths were spelled `/tmp`, a POSIX convention in a program that also builds
// for Windows, and nothing removed them: a run left ten files and most of a megabyte behind, and every run
// after it overwrote the same ten names. The lifetime is the tests' scratch file's, and the declaration has to
// come before the streams it names: destruction runs in reverse, so the files are closed before they are
// removed, which is what a platform that refuses to unlink an open file requires.
class ScrapOutputs
{
   public:
    explicit ScrapOutputs(const std::string& strategy)
        : orders_(std::filesystem::temp_directory_path() /
                  ("toy_quant_benchmark_orders_" + strategy + ".csv")),
          trades_(std::filesystem::temp_directory_path() /
                  ("toy_quant_benchmark_trades_" + strategy + ".csv"))
    {
    }

    ~ScrapOutputs()
    {
        std::error_code error;
        std::filesystem::remove(orders_, error);
        std::filesystem::remove(trades_, error);
    }

    ScrapOutputs(const ScrapOutputs&) = delete;
    ScrapOutputs& operator=(const ScrapOutputs&) = delete;

    const std::filesystem::path& orders() const { return orders_; }
    const std::filesystem::path& trades() const { return trades_; }

   private:
    std::filesystem::path orders_;
    std::filesystem::path trades_;
};
}  // namespace

std::vector<StrategyBenchmarkResult> run_strategy_benchmark(const std::string& trades_path,
                                                            const std::string& quotes_path,
                                                            const std::string& symbol,
                                                            uint64_t quantity_scale,
                                                            const L1StrategyConfig& l1_config,
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
        // Declared before the streams it names, so that it is destroyed after them.
        const ScrapOutputs scrap(name);
        std::ofstream orders(scrap.orders());
        std::ofstream trades(scrap.trades());
        if (!orders || !trades)
            throw std::runtime_error("failed to open temporary benchmark files for strategy: " + name);

        orders << "ts,symbol,side,price,quantity,order_id\n";
        trades << "ts,symbol,side,price,quantity,order_id,liquidity_role,fee\n";

        auto strategy = make_strategy(
            name, StrategyFactoryConfig{.instrument = &instrument, .l1 = l1_config});
        // The schedule comes off the instrument through the one function that reads it, which is the rule
        // its own comment states: a term listed by hand at a construction site is a term that stops being
        // applied the day it is added to the instrument, and the run then prices it at zero in silence.
        //
        // The queue arguments after it are the engine's defaults, and on this path they are inert: the L1
        // line's feed carries trades and a best bid and offer with no depth, so there is no level for a
        // queue to be attached to. A queue model here would be a knob that changes nothing, which is why
        // the L1 comparisons can be read as a price-touch model at all.
        MatchingEngine engine(instrument.tick_size, fee_schedule_of(instrument), 1, QueueModel::ProRata, 1,
                              kDefaultQueueSeed, 1.0, 0, 0, instrument.lot_size,
                              instrument.min_order_quantity);
        Portfolio portfolio(instrument.quantity_scale, 1000.0, instrument.unit_notional_usd,
                        instrument.multiplier);
        Pipeline pipeline(orders, trades, *strategy, engine, portfolio, instrument.tick_size);

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
