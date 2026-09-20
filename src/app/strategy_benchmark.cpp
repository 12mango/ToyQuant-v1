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
                                                            uint64_t quantity_scale)
{
    if (symbol.empty()) throw std::invalid_argument("benchmark symbol cannot be empty");

    const InstrumentSpec instrument = btc_usdt_spec(quantity_scale);
    const std::vector<std::string> names = {"passive_l1", "inventory_aware_l1",
                                            "flow_aware_l1", "l1"};
    std::vector<StrategyBenchmarkResult> results;
    results.reserve(names.size());

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

        auto strategy = make_strategy(name, &instrument);
        OrderBook order_book(instrument.tick_size);
        MatchingEngine engine(nullptr, instrument.tick_size,
                              FeeSchedule{.maker_rate = instrument.maker_fee_rate,
                                          .taker_rate = instrument.taker_fee_rate,
                                          .quantity_scale = instrument.quantity_scale});
        Portfolio portfolio(instrument.quantity_scale);
        Logger logger;
        Pipeline pipeline(orders, trades, order_book, *strategy, engine, portfolio, logger);

        double final_mid = 0.0;
        ReplayFeed feed(make_market_data_readers("binance", trades_path, quotes_path, instrument),
                       [&](const MarketEvent& event)
                       {
                           if (const auto* quote = std::get_if<BboQuote>(&event))
                               final_mid = (quote->bid_price + quote->ask_price) / 2.0;
                           pipeline.process_event(event);
                       },
                       0);
        feed.run();
        if (final_mid > 0.0) portfolio.mark_to_market({{symbol, final_mid}});
        const auto summary = pipeline.summary();
        const auto& validation = feed.validation_summary();
        StrategyBenchmarkResult result;
        result.strategy_name = name;
        result.submitted_orders = summary.submitted_orders;
        result.submitted_quantity = summary.submitted_quantity;
        result.trade_reports = summary.trade_reports;
        result.filled_quantity = summary.trade_report_quantity;
        result.cancel_requests = summary.cancel_requests;
        result.net_position = strategy->net_position();
        result.fill_rate = summary.fill_rate;
        result.cancel_rate = summary.cancel_rate;
        result.realized_pnl = summary.portfolio.realized_pnl;
        result.equity = summary.portfolio.equity;
        result.fees_paid = summary.portfolio.fees_paid;
        result.working_orders = summary.working_orders;
        result.stale_trades = validation.stale_trades;
        result.dislocated_trades = validation.dislocated_trades;
        result.max_trade_deviation_bps = validation.max_trade_deviation_bps;
        results.push_back(result);
    }

    return results;
}
