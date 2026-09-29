// The checks in this test are the test: keep them enabled even when the build defines
// NDEBUG, which is the case for the RelWithDebInfo profiling preset.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include "accounting/portfolio.h"
#include "app/pipeline.h"
#include "exchange/matching_engine.h"
#include "orderbook/orderbook.h"
#include "strategy/l2_market_maker.h"
#include "strategy/market_maker.h"
#include "utils/logger.h"

int main()
{
    std::ofstream orders("/tmp/pipeline_accounting_orders.csv");
    std::ofstream trades("/tmp/pipeline_accounting_trades.csv");
    NaiveMarketMaker strategy(10, 0.1, 1.0);
    OrderBook order_book(1.0);
    MatchingEngine engine(1.0,
                          FeeSchedule{.maker_rate = 0.001,
                                      .taker_rate = 0.002,
                                      .quantity_scale = 1000});
    Portfolio portfolio(1000, 1000.0);
    Logger logger;
    Pipeline pipeline(orders, trades, order_book, strategy, engine, portfolio);

    pipeline.process_event(BboQuote{1, "TEST", 99.0, 100, 101.0, 100, 1, "binance"});
    pipeline.process_event(MarketTrade{2, "TEST", 100.0, 10, Side::Sell, 2, "binance"});

    const auto* position = portfolio.find_position("TEST");
    const auto metrics = portfolio.metrics();
    assert(position != nullptr);
    assert(position->quantity == 10);
    assert(metrics.maker_trade_count == 1);
    assert(metrics.taker_trade_count == 0);
    assert(std::abs(metrics.fees_paid - 0.001) < 1e-12);
    assert(pipeline.summary().trade_reports == 1);

    Pipeline filtered_pipeline(orders, trades, order_book, strategy, engine, portfolio);
    filtered_pipeline.process_event(MarketTrade{3, "TEST", 110.0, 10, Side::Sell, 3, "binance"});
    assert(filtered_pipeline.summary().trade_reports == 0);
    assert(filtered_pipeline.summary().filtered_market_trades == 1);

    Pipeline isolated_pipeline(orders, trades, order_book, strategy, engine, portfolio);
    isolated_pipeline.process_event(BboQuote{4, "TEST", 99.0, 100, 101.0, 100, 4, "binance"});
    bool rejected_mixed_exchange = false;
    try
    {
        isolated_pipeline.process_event(MarketDepthSnapshot{
            .ts = 5,
            .symbol = "BTC-PERPETUAL",
            .sequence = 1,
            .bids = {{100.0, 1}},
            .asks = {{101.0, 1}},
            .exchange = "deribit",
        });
    }
    catch (const std::invalid_argument&)
    {
        rejected_mixed_exchange = true;
    }
    assert(rejected_mixed_exchange);

    std::ofstream l2_orders("/tmp/pipeline_l2_queue_orders.csv");
    std::ofstream l2_trades("/tmp/pipeline_l2_queue_trades.csv");
    L2MarketMaker l2_strategy(L2MarketMakerConfig{.order_size = 10,
                                                   .base_spread = 0.5,
                                                   .inventory_limit = 100,
                                                   .tick_size = 1.0,
                                                   .signal_mode = L2SignalMode::Baseline});
    OrderBook l2_execution_book(1.0);
    MatchingEngine l2_engine(1.0);
    Portfolio l2_portfolio;
    Pipeline l2_pipeline(l2_orders, l2_trades, l2_execution_book, l2_strategy, l2_engine,
                         l2_portfolio);
    l2_pipeline.set_trace_order(1);
    l2_pipeline.process_l2_market_view(L2MarketView{.symbol = "L2",
                                                    .ts = 10,
                                                    .top = TopOfBook{100.0, 100, 101.0, 100},
                                                    .micro_price = 100.5});
    assert(l2_pipeline.summary().submitted_orders == 2);
    l2_pipeline.process_l2_market_trade(
        MarketTrade{11, "L2", 100.0, 50, Side::Sell, 1, "deribit"});
    assert(l2_pipeline.summary().trade_reports == 0);
    l2_pipeline.process_l2_market_trade(
        MarketTrade{12, "L2", 100.0, 60, Side::Sell, 2, "deribit"});
    assert(l2_pipeline.summary().trade_reports == 1);
    assert(l2_pipeline.summary().trade_report_quantity == 10);
    assert(l2_pipeline.summary().queue_ahead_consumed == 100);

    // The trace of order 1, which is the bid the first view submitted. It joins behind the 100 the level
    // displays, the first market trade takes half of that queue, and the second reaches it and fills 10.
    // The accessor behind these rows reports the order's wait, not a property of the price, so the submit
    // row has no queue and the rest row is where the joined queue appears.
    const auto trace = l2_pipeline.summary().queue_trace;
    assert(trace.size() == 5);
    assert(trace[0].event == "submit" && trace[0].side == Side::Buy && trace[0].price == 100.0);
    // The submit row has no queue because the order is not in a level yet, which is the accessor's real
    // semantics rather than a gap in it.
    assert(trace[0].change == 10 && trace[0].queue_ahead == 0);
    assert(trace[1].event == "rest" && trace[1].queue_ahead == 100);
    assert(trace[2].event == "trade" && trace[2].queue_ahead == 50 && trace[2].change == 50);
    // The queue reached zero and the order traded. The second market trade leaves the queue where it is, so
    // it produces no further row: a row means the queue moved.
    assert(trace[3].event == "fill" && trace[3].change == 10 && trace[3].queue_ahead == 0);
    assert(trace[4].event == "fully_filled" && trace[4].queue_ahead == 0);
    assert(l2_pipeline.summary().first_fill_order_id == 1);
    assert(l2_pipeline.summary().trace_requested);

    MatchingEngine deribit_fee_engine(0.5,
        FeeSchedule{.maker_rate = 0.0002,
                    .taker_rate = 0.0005,
                    .quantity_scale = 1,
                    .unit_notional_usd = 10.0});
    std::ofstream fee_orders("/tmp/pipeline_deribit_fee_orders.csv");
    std::ofstream fee_trades("/tmp/pipeline_deribit_fee_trades.csv");
    NaiveMarketMaker fee_strategy(1, 0.0, 0.5);
    OrderBook fee_book(0.5);
    Portfolio fee_portfolio(1, 1000.0, 10.0);
    Pipeline fee_pipeline(fee_orders, fee_trades, fee_book, fee_strategy, deribit_fee_engine,
                          fee_portfolio);
    fee_pipeline.process_event(BboQuote{20, "BTC-PERPETUAL", 99.0, 100, 101.0, 100, 1,
                                        "deribit"});
    fee_pipeline.process_event(
        MarketTrade{21, "BTC-PERPETUAL", 100.0, 1, Side::Sell, 2, "deribit"});
    // One Deribit BTC-PERPETUAL contract is worth 10 USD, so the maker fee is 0.02% of 10 USD and
    // not 0.02% of the quoted price.
    assert(std::abs(fee_portfolio.metrics().fees_paid - 0.002) < 1e-12);
    // A pipeline that was never asked to trace an order reports no hints, which is what keeps the summary
    // of an ordinary run exactly as it was.
    assert(!fee_pipeline.summary().trace_requested);
}