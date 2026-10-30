#include <cassert>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include "accounting/portfolio.h"
#include "app/pipeline.h"
#include "exchange/matching_engine.h"
#include "market/funding_rate.h"
#include "orderbook/top_of_book.h"
#include "strategy/l2_market_maker_base.h"
#include "strategy/baseline_market_makers.h"
#include "support/test_support.h"
#include "utils/logger.h"

int main()
{
    test_support::TempFile orders_file("pipeline_accounting_orders.csv");
    test_support::TempFile trades_file("pipeline_accounting_trades.csv");
    std::ofstream orders = orders_file.open();
    std::ofstream trades = trades_file.open();
    NaiveMarketMaker strategy(10, 0.1, 1.0);
    MatchingEngine engine(1.0,
                          FeeSchedule{.maker_rate = 0.001,
                                      .taker_rate = 0.002,
                                      .quantity_scale = 1000});
    Portfolio portfolio(1000, 1000.0);
    Logger logger;
    Pipeline pipeline(orders, trades, strategy, engine, portfolio);

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

    Pipeline filtered_pipeline(orders, trades, strategy, engine, portfolio);
    filtered_pipeline.process_event(MarketTrade{3, "TEST", 110.0, 10, Side::Sell, 3, "binance"});
    assert(filtered_pipeline.summary().trade_reports == 0);
    assert(filtered_pipeline.summary().filtered_market_trades == 1);

    Pipeline isolated_pipeline(orders, trades, strategy, engine, portfolio);
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

    test_support::TempFile l2_orders_file("pipeline_l2_queue_orders.csv");
    test_support::TempFile l2_trades_file("pipeline_l2_queue_trades.csv");
    std::ofstream l2_orders = l2_orders_file.open();
    std::ofstream l2_trades = l2_trades_file.open();
    L2MarketMakerBase l2_strategy(L2MarketMakerBaseConfig{.order_size = 10,
                                                   .base_spread = 0.5,
                                                   .inventory_limit = 100,
                                                   .tick_size = 1.0,
                                                   .signal_mode = L2SignalMode::Baseline});
    MatchingEngine l2_engine(1.0);
    Portfolio l2_portfolio;
    Pipeline l2_pipeline(l2_orders, l2_trades, l2_strategy, l2_engine, l2_portfolio);
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

    // The distribution over orders, which is the statistic the trace illustrates. Order 1 (the bid) filled,
    // so its closest approach and its queue at the end are both zero. Order 2 is still resting, so it has no
    // profile yet: the profile is closed when an order dies, not when it is submitted.
    assert(l2_pipeline.summary().queue_profile_orders == 1);
    assert(l2_pipeline.summary().queue_zero_orders == 1);
    assert(l2_pipeline.summary().queue_min_fraction_p50 == 0.0);
    assert(l2_pipeline.summary().queue_end_fraction_p50 == 0.0);

    // A view that moves the mid far enough to requote cancels the ask, whose queue was never touched. This is
    // the case that proves the end-of-life reading is taken before the cancel: the engine erases a level once
    // no order rests in it, so a reading taken after the Cancelled report would report zero here as well and
    // the two percentiles would be indistinguishable.
    l2_pipeline.process_l2_market_view(L2MarketView{.symbol = "L2",
                                                    .ts = 13,
                                                    .top = TopOfBook{101.5, 100, 102.5, 100},
                                                    .micro_price = 102.0});
    assert(l2_pipeline.summary().queue_profile_orders == 2);
    assert(l2_pipeline.summary().queue_zero_orders == 1);
    assert(l2_pipeline.summary().queue_min_fraction_p50 == 1.0);
    assert(l2_pipeline.summary().queue_end_fraction_p50 == 1.0);

    // A normal run never has an order refused: the guard, the lot size and the minimum are all checked on
    // every submit, and a refusal would show up here rather than as a missing quote.
    assert(l2_engine.rejected_orders() == 0);

    MatchingEngine deribit_fee_engine(0.5,
        FeeSchedule{.maker_rate = 0.0002,
                    .taker_rate = 0.0005,
                    .quantity_scale = 1,
                    .unit_notional_usd = 10.0});
    test_support::TempFile fee_orders_file("pipeline_deribit_fee_orders.csv");
    test_support::TempFile fee_trades_file("pipeline_deribit_fee_trades.csv");
    std::ofstream fee_orders = fee_orders_file.open();
    std::ofstream fee_trades = fee_trades_file.open();
    NaiveMarketMaker fee_strategy(1, 0.0, 0.5);
    Portfolio fee_portfolio(1, 1000.0, 10.0);
    Pipeline fee_pipeline(fee_orders, fee_trades, fee_strategy, deribit_fee_engine, fee_portfolio);
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

    // Funding as an input to the ledger. The published rate is what longs pay and the accounting takes the
    // rate a long earns, so a settlement is a cost to a long position and the pipeline is what flips the
    // sign. The timing rule is the other half: a settlement is applied at the first event at or after it, on
    // the position the previous event left, at the last price the run has seen.
    {
        FundingRateSeries funding("BTC-PERPETUAL");
        funding.add(0, 0.5);      // reached while the account is flat and no price exists yet
        funding.add(30, 0.0001);  // 0.01%, which the long pays
        test_support::TempFile funding_orders_file("pipeline_funding_orders.csv");
        test_support::TempFile funding_trades_file("pipeline_funding_trades.csv");
        std::ofstream funding_orders = funding_orders_file.open();
        std::ofstream funding_trades = funding_trades_file.open();
        NaiveMarketMaker funding_strategy(1, 0.0, 0.5);
        MatchingEngine funding_engine(0.5, FeeSchedule{.maker_rate = 0.0002,
                                                       .taker_rate = 0.0005,
                                                       .quantity_scale = 1,
                                                       .unit_notional_usd = 10.0});
        Portfolio funding_portfolio(1, 1000.0, 10.0);
        Pipeline funding_pipeline(funding_orders, funding_trades, funding_strategy, funding_engine,
                                  funding_portfolio);
        funding_pipeline.set_funding_rates(&funding);

        funding_pipeline.process_event(BboQuote{20, "BTC-PERPETUAL", 99.0, 100, 101.0, 100, 1,
                                                "deribit"});
        funding_pipeline.process_event(
            MarketTrade{21, "BTC-PERPETUAL", 100.0, 1, Side::Sell, 2, "deribit"});
        // The settlement at ts=0 was reached by the first event, before any price and with a flat account:
        // applied, and worth nothing. The one at ts=30 is still ahead of the clock.
        assert(funding_pipeline.summary().funding.settlements == 1);
        assert(std::abs(funding_pipeline.summary().funding.flow_usd) < 1e-15);

        const double cash_before = funding_portfolio.metrics().cash;
        funding_pipeline.process_event(BboQuote{30, "BTC-PERPETUAL", 99.0, 100, 101.0, 100, 3,
                                                "deribit"});
        // One contract is 10 USD of face value, the mark is the last mid the run saw (100), and the long
        // pays 0.01% of the notional: 0.001 USD out of cash.
        assert(funding_pipeline.summary().funding.settlements == 2);
        assert(std::abs(funding_pipeline.summary().funding.flow_usd + 0.001) < 1e-12);
        assert(std::abs(funding_portfolio.metrics().cash - (cash_before - 0.001)) < 1e-12);
        // The line reports the file as well as the settlements, and exists only because a series was given.
        assert(funding_pipeline.summary().funding_requested);
        assert(funding_pipeline.summary().funding.points == 2);
        assert(std::abs(funding_pipeline.summary().funding.sum_rate - 0.5001) < 1e-15);
        assert(!fee_pipeline.summary().funding_requested);
    }

    // The adverse-selection measurement: the pipeline resolves what a passive fill at each distance from the
    // midpoint would have been worth, from the venue's own prints, and pushes it to the strategy. The strategy
    // cannot measure it, because it never sees the midpoint after a fill, and the [GATE] line is printed only
    // when a run asks for it.
    {
        test_support::TempFile print_orders_file("pipeline_print_orders.csv");
        test_support::TempFile print_trades_file("pipeline_print_trades.csv");
        std::ofstream print_orders = print_orders_file.open();
        std::ofstream print_trades = print_trades_file.open();
        L2MarketMakerBase print_strategy(L2MarketMakerBaseConfig{.order_size = 1,
                                                         .base_spread = 1.0,
                                                         .inventory_limit = 10,
                                                         .tick_size = 1.0});
        MatchingEngine print_engine(1.0);
        Portfolio print_portfolio;
        Pipeline print_pipeline(print_orders, print_trades, print_strategy, print_engine,
                                print_portfolio);
        print_pipeline.set_markout_cover(true);
        print_pipeline.set_gate_diagnostics(true);

        const auto view_at = [](uint64_t ts, double mid) {
            return L2MarketView{.symbol = "L2",
                                .ts = ts,
                                .top = TopOfBook{mid - 0.5, 10, mid + 0.5, 10},
                                .micro_price = mid};
        };
        print_pipeline.process_l2_market_view(view_at(10, 100.0));
        assert(print_pipeline.summary().gate.market_samples == 0);
        // A passive buy at the midpoint, then five cycles with the midpoint a tick higher: the print was worth a
        // tick to whoever was resting there, which is the sign the estimate is built from.
        print_pipeline.process_l2_market_trade(
            MarketTrade{11, "L2", 100.0, 10, Side::Sell, 1, "deribit"});
        for (uint64_t ts = 12; ts <= 17; ++ts) print_pipeline.process_l2_market_view(view_at(ts, 101.0));
        const auto printed = print_pipeline.summary();
        assert(printed.gate_diagnostics_requested);
        assert(printed.gate.market_samples == 1);
        assert(printed.gate.market_buy_near_ticks > 0.0);
    }

    // The round-trip distribution: a win is a round trip whose net PnL is above zero, the profit factor is
    // the winning sum over the absolute losing one, and the holding time runs from the position's opening
    // fill. The third trade below is gross-positive and net-negative, which is the case the fee-inclusive
    // definition exists for.
    {
        const std::vector<ClosedTrade> trades = {
            {.symbol = "X",
             .side = exchange::Side::Buy,
             .entry_price = 100.0,
             .exit_price = 101.0,
             .quantity = 1.0,
             .pnl = 1.0,
             .fee = 0.2,
             .entry_ts = 0,
             .exit_ts = 2'000'000},
            {.symbol = "X",
             .side = exchange::Side::Sell,
             .entry_price = 101.0,
             .exit_price = 102.0,
             .quantity = 1.0,
             .pnl = -1.0,
             .fee = 0.2,
             .entry_ts = 3'000'000,
             .exit_ts = 4'000'000},
            {.symbol = "X",
             .side = exchange::Side::Buy,
             .entry_price = 100.0,
             .exit_price = 100.1,
             .quantity = 1.0,
             .pnl = 0.1,
             .fee = 0.2,
             .entry_ts = 5'000'000,
             .exit_ts = 5'500'000},
        };
        const auto round_trips = RunAccounting::round_trip_summary(trades, 20.0, 30.0, 10.0);
        assert(round_trips.trades == 3);
        assert(std::abs(round_trips.win_rate - 1.0 / 3.0) < 1e-12);
        // The one win is 0.8 net and the two losses are -1.2 and -0.1, so the factor is 0.8 / 1.3.
        assert(std::abs(round_trips.profit_factor - 0.8 / 1.3) < 1e-12);
        assert(std::abs(round_trips.expectancy_usd + 0.5 / 3.0) < 1e-12);
        assert(std::abs(round_trips.average_win_usd - 0.8) < 1e-12);
        assert(std::abs(round_trips.average_loss_usd + 0.65) < 1e-12);
        // Two seconds, one second and half a second over three round trips.
        assert(std::abs(round_trips.average_holding_seconds - 3.5 / 3.0) < 1e-12);
        assert(std::abs(round_trips.taker_notional_usd - 20.0) < 1e-12);
        assert(std::abs(round_trips.total_notional_usd - 30.0) < 1e-12);
        assert(std::abs(round_trips.turnover - 3.0) < 1e-12);

        // A run that never closed a position reports no distribution and still reports its turnover; a run
        // with no collateral has no turnover to state.
        const auto empty = RunAccounting::round_trip_summary({}, 0.0, 0.0, 0.0);
        assert(empty.trades == 0);
        assert(empty.win_rate == 0.0);
        assert(empty.turnover == 0.0);
    }

    // The taker measurements: the arrival slippage against the decision price, the drift at the three fixed
    // horizons, and the shortfall of an aggressive order whose remainder never filled.
    {
        RunAccounting taker;
        taker.note_taker_fill({.side = Side::Buy,
                               .price = 101.0,
                               .quantity = 10,
                               .slippage_ticks = 1.0,
                               .slippage_bps = 99.0,
                               .slippage_usd = 0.01,
                               .quote_cycle = 100});
        // Only the five-cycle horizon has closed at 105; the other two are still queued.
        taker.drain_taker_measurements(105, 103.0, 1.0, 0.0, 1.0, 1);
        assert(std::abs(taker.taker_summary().edge_5_ticks - 2.0) < 1e-12);
        assert(taker.taker_summary().edge_20_ticks == 0.0);
        taker.drain_taker_measurements(120, 104.0, 1.0, 0.0, 1.0, 1);
        assert(std::abs(taker.taker_summary().edge_20_ticks - 3.0) < 1e-12);
        // By the sixtieth cycle the mid is back below the fill price, which is a negative edge: the drift
        // did not cover what the crossing paid.
        taker.drain_taker_measurements(160, 100.0, 1.0, 0.0, 1.0, 1);
        const auto summary = taker.taker_summary();
        assert(std::abs(summary.edge_60_ticks + 1.0) < 1e-12);
        assert(summary.fills == 1);
        assert(std::abs(summary.arrival_slippage_ticks - 1.0) < 1e-12);
        assert(std::abs(summary.arrival_slippage_bps - 99.0) < 1e-12);
        assert(std::abs(summary.arrival_slippage_usd - 0.01) < 1e-12);
    }

    // An aggressive buy of ten units that filled six a tick above its decision midpoint and never got the
    // other four: the filled part costs 6 USD, the fee 0.6, and the missed four would have earned 8 USD by
    // the horizon, so the shortfall is 14.6 USD against a thousand dollar decision notional.
    {
        RunAccounting shortfall;
        shortfall.note_aggressive_order({.side = Side::Buy,
                                         .decision_mid = 100.0,
                                         .ordered_quantity = 10,
                                         .filled_quantity = 6,
                                         .filled_usd = 6.0,
                                         .fees_usd = 0.6,
                                         .decision_usd = 1000.0,
                                         .start_cycle = 100});
        shortfall.drain_taker_measurements(105, 102.0, 1.0, 0.0, 1.0, 1);
        const auto summary = shortfall.taker_summary();
        assert(summary.aggressive_orders == 1);
        assert(std::abs(summary.shortfall_filled_usd - 6.0) < 1e-12);
        assert(std::abs(summary.shortfall_fees_usd - 0.6) < 1e-12);
        assert(std::abs(summary.shortfall_opportunity_usd - 8.0) < 1e-12);
        assert(std::abs(summary.shortfall_usd - 14.6) < 1e-12);
        assert(std::abs(summary.shortfall_bps - 146.0) < 1e-12);
    }
}