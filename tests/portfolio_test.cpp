#include "accounting/portfolio.h"

#include <cassert>
#include <cmath>
#include <unordered_map>

int main()
{
    Portfolio portfolio(100, 1000.0);
    portfolio.apply(ExecutionReport{.side = exchange::Side::Buy,
                                    .exec_type = ExecType::Trade,
                                    .symbol = "TEST",
                                    .price = 10.0,
                                    .quantity = 100,
                                    .owner = "test",
                                    .liquidity_role = LiquidityRole::Maker,
                                    .fee = 0.01});

    const auto* position = portfolio.find_position("TEST");
    assert(position != nullptr);
    assert(position->quantity == 100);
    assert(std::abs(position->average_price - 10.0) < 1e-12);
    assert(std::abs(portfolio.metrics().fees_paid - 0.01) < 1e-12);

    portfolio.apply(ExecutionReport{.side = exchange::Side::Sell,
                                    .exec_type = ExecType::Trade,
                                    .symbol = "TEST",
                                    .price = 11.0,
                                    .quantity = 50,
                                    .owner = "test",
                                    .liquidity_role = LiquidityRole::Taker,
                                    .fee = 0.005});
    position = portfolio.find_position("TEST");
    assert(position->quantity == 50);
    assert(std::abs(portfolio.metrics().realized_pnl - 0.485) < 1e-12);
    assert(std::abs(portfolio.metrics().maker_fees - 0.01) < 1e-12);
    assert(std::abs(portfolio.metrics().taker_fees - 0.005) < 1e-12);

    portfolio.mark_to_market({{"TEST", 12.0}});
    const auto metrics = portfolio.metrics();
    assert(std::abs(metrics.unrealized_pnl - 1.0) < 1e-12);
    assert(std::abs(metrics.equity - 1001.485) < 1e-12);

    // equity_at is the read-only companion of mark_to_market: the same prices give the same equity, a
    // different mark gives a different one, and the account's own stored mark does not move.
    assert(std::abs(portfolio.equity_at({{"TEST", 12.0}}) - 1001.485) < 1e-12);
    assert(std::abs(portfolio.equity_at({{"TEST", 13.0}}) - 1001.985) < 1e-12);
    assert(std::abs(portfolio.metrics().equity - 1001.485) < 1e-12);
    assert(std::abs(portfolio.metrics().unrealized_pnl - 1.0) < 1e-12);
    // Round-trip recording is off unless it is asked for, and this account never asked.
    assert(portfolio.closed_trades().empty());

    // A closing fill becomes one round trip when recording is on: the side the position was opened on,
    // the position's own entry price and time, the closing portion of the PnL, and the share of the
    // fill's fee that belongs to the quantity it closed.
    {
        Portfolio book(100, 1000.0);
        book.record_closed_trades(true);
        book.apply(ExecutionReport{.side = exchange::Side::Buy,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "TEST",
                                   .price = 10.0,
                                   .quantity = 100,
                                   .ts = 1'000'000,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Taker,
                                   .fee = 0.02});
        // Opening a position is not a round trip.
        assert(book.closed_trades().empty());
        book.apply(ExecutionReport{.side = exchange::Side::Sell,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "TEST",
                                   .price = 11.0,
                                   .quantity = 40,
                                   .ts = 4'000'000,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Taker,
                                   .fee = 0.008});
        assert(book.closed_trades().size() == 1);
        const ClosedTrade& trip = book.closed_trades().front();
        assert(trip.symbol == "TEST");
        assert(trip.side == exchange::Side::Buy);
        assert(std::abs(trip.entry_price - 10.0) < 1e-12);
        assert(std::abs(trip.exit_price - 11.0) < 1e-12);
        assert(std::abs(trip.quantity - 0.4) < 1e-12);
        assert(std::abs(trip.pnl - 0.4) < 1e-12);
        // The exit half is the sell's own fee (0.008) and the entry half is 40% of the 0.02 the open
        // paid, so the round trip is charged both: what it cost to get in and what it cost to get out.
        assert(std::abs(trip.fee - 0.016) < 1e-12);
        assert(trip.entry_ts == 1'000'000);
        assert(trip.exit_ts == 4'000'000);
        // The close claimed its share of the entry fees and the rest stays with the quantity still held.
        const auto* remaining = book.find_position("TEST");
        assert(remaining != nullptr);
        assert(std::abs(remaining->entry_fees - 0.012) < 1e-12);

        // Adding to a position that is still open keeps its opening moment, which is what a holding time
        // is measured from; only a position that went flat starts a new clock.
        book.apply(ExecutionReport{.side = exchange::Side::Buy,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "TEST",
                                   .price = 11.0,
                                   .quantity = 60,
                                   .ts = 5'000'000,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Taker,
                                   .fee = 0.012});
        const auto* held = book.find_position("TEST");
        assert(held != nullptr);
        assert(held->quantity == 120);
        assert(held->entry_ts == 1'000'000);
    }

    // A contract with a fixed USD face value holds that USD amount of the base asset, bought at
    // the entry price. One Deribit BTC-PERPETUAL contract is 10 USD, so a 10% price move on one
    // contract gains 10 USD * 10% = 1 USD. Valuing the contract at the BTC price instead would
    // report a gain of roughly 637 USD.
    {
        Portfolio perp(1, 1000.0, 10.0);
        perp.apply(ExecutionReport{.side = exchange::Side::Buy,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "BTC-PERPETUAL",
                                   .price = 6000.0,
                                   .quantity = 1,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Maker,
                                   .fee = 0.002});
        const auto* long_position = perp.find_position("BTC-PERPETUAL");
        assert(long_position != nullptr);
        assert(long_position->quantity == 1);
        assert(std::abs(long_position->average_price - 6000.0) < 1e-12);
        // 1000 minus the 10 USD contract value and the 0.002 fee.
        assert(std::abs(perp.metrics().cash - 989.998) < 1e-9);
        assert(std::abs(perp.metrics().realized_pnl + 0.002) < 1e-9);

        perp.mark_to_market({{"BTC-PERPETUAL", 6600.0}});
        assert(std::abs(perp.metrics().unrealized_pnl - 1.0) < 1e-9);
        assert(std::abs(perp.metrics().equity - 1000.998) < 1e-9);

        perp.apply(ExecutionReport{.side = exchange::Side::Sell,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "BTC-PERPETUAL",
                                   .price = 6600.0,
                                   .quantity = 1,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Taker,
                                   .fee = 0.005});
        assert(perp.find_position("BTC-PERPETUAL")->quantity == 0);
        // Start at 1000, gain 1 USD on the 10 USD exposure, pay 0.002 + 0.005 of fees.
        assert(std::abs(perp.metrics().realized_pnl - 0.993) < 1e-9);
        assert(std::abs(perp.metrics().cash - 1000.993) < 1e-9);
        perp.mark_to_market({{"BTC-PERPETUAL", 6600.0}});
        assert(std::abs(perp.metrics().equity - 1000.993) < 1e-9);
    }

    // The same contract on the short side loses 1 USD when the price rises 10%.
    {
        Portfolio perp(1, 1000.0, 10.0);
        perp.apply(ExecutionReport{.side = exchange::Side::Sell,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "BTC-PERPETUAL",
                                   .price = 6000.0,
                                   .quantity = 1,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Maker,
                                   .fee = 0.002});
        assert(std::abs(perp.metrics().cash - 1009.998) < 1e-9);
        perp.mark_to_market({{"BTC-PERPETUAL", 6600.0}});
        assert(std::abs(perp.metrics().unrealized_pnl + 1.0) < 1e-9);
        assert(std::abs(perp.metrics().equity - 998.998) < 1e-9);
        assert(perp.find_position("BTC-PERPETUAL")->quantity == -1);

        perp.apply(ExecutionReport{.side = exchange::Side::Buy,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "BTC-PERPETUAL",
                                   .price = 6600.0,
                                   .quantity = 1,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Taker,
                                   .fee = 0.005});
        assert(std::abs(perp.metrics().realized_pnl + 1.007) < 1e-9);
        assert(std::abs(perp.metrics().cash - 998.993) < 1e-9);
    }

    // One trade that both closes a position and opens the opposite one. The closing part is valued from the
    // entry price and the opening part at the trade price, and the new side's average price has to be the
    // trade price alone: blending it with the closed side's entry is the classic form of this bug, and it is
    // silent because every total still looks plausible afterwards.
    {
        Portfolio flip(1, 1000.0, 10.0);
        // Long two contracts at 6000, so cash pays 20 USD of notional and the position is +2.
        flip.apply(ExecutionReport{.side = exchange::Side::Buy,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "BTC-PERPETUAL",
                                   .price = 6000.0,
                                   .quantity = 2,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Maker,
                                   .fee = 0.0});
        assert(flip.find_position("BTC-PERPETUAL")->quantity == 2);
        assert(std::abs(flip.metrics().cash - 980.0) < 1e-9);

        // Sell five at 6600: two close the long, three open a short.
        flip.apply(ExecutionReport{.side = exchange::Side::Sell,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "BTC-PERPETUAL",
                                   .price = 6600.0,
                                   .quantity = 5,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Taker,
                                   .fee = 0.0});
        const auto* short_position = flip.find_position("BTC-PERPETUAL");
        assert(short_position->quantity == -3);
        assert(std::abs(short_position->average_price - 6600.0) < 1e-9);
        // 20 USD of exposure moved 10%, so the closed part realises 2 USD.
        assert(std::abs(flip.metrics().realized_pnl - 2.0) < 1e-9);
        // 980 in cash, plus the 22 received for the closed long, plus the 30 notional of the new short.
        assert(std::abs(flip.metrics().cash - 1032.0) < 1e-9);

        // Marked at the short's own entry price, equity is the starting cash plus what has been realised.
        flip.mark_to_market({{"BTC-PERPETUAL", 6600.0}});
        assert(std::abs(flip.metrics().unrealized_pnl) < 1e-9);
        assert(std::abs(flip.metrics().equity - 1002.0) < 1e-9);

        // A move back to 6000 gains on the short: 30 USD of exposure, marked at 6000/6600 of its entry.
        flip.mark_to_market({{"BTC-PERPETUAL", 6000.0}});
        assert(std::abs(flip.metrics().unrealized_pnl - 2.727272727) < 1e-6);
        assert(std::abs(flip.metrics().equity - 1004.727272727) < 1e-6);
    }

    // A per-interval cash flow on a position whose USD value comes from the traded price, which is the
    // dividend and stock-financing case. The notional is the position marked at the price, so two units
    // at 100 is 200 USD and the rate applies to that rather than to the entry price. A rate the position
    // receives is positive, so 0.5% per interval credits 1 USD and four intervals credit 4.
    {
        Portfolio stock(1, 1000.0);
        stock.apply(ExecutionReport{.side = exchange::Side::Buy,
                                    .exec_type = ExecType::Trade,
                                    .symbol = "STOCK",
                                    .price = 100.0,
                                    .quantity = 2,
                                    .owner = "test",
                                    .liquidity_role = LiquidityRole::Maker,
                                    .fee = 0.0});
        stock.mark_to_market({{"STOCK", 100.0}});
        assert(std::abs(stock.metrics().equity - 1000.0) < 1e-9);

        double credited = 0.0;
        for (int interval = 0; interval < 4; ++interval)
        {
            credited += stock.accrue_interval_flow("STOCK", 0.005, 100.0);
        }
        assert(std::abs(credited - 4.0) < 1e-9);
        assert(std::abs(stock.metrics().cash - 804.0) < 1e-9);
        assert(std::abs(stock.metrics().realized_pnl - 4.0) < 1e-9);
        // The position is untouched and still worth 200 USD, so equity is cash plus that 200.
        assert(std::abs(stock.metrics().equity - 1004.0) < 1e-9);

        // The rate applies to the mark, not to the entry: the same 0.5% at a doubled mark is 2 USD on
        // 400 USD of notional. A flow is cash, so it is realized at once rather than only on a close.
        assert(std::abs(stock.accrue_interval_flow("STOCK", 0.005, 200.0) - 2.0) < 1e-9);
        assert(std::abs(stock.metrics().cash - 806.0) < 1e-9);
        assert(std::abs(stock.metrics().realized_pnl - 6.0) < 1e-9);
        assert(std::abs(stock.metrics().equity - 1006.0) < 1e-9);
    }

    // The same call with a negative rate is a position being charged rather than paid, which is the
    // financing a long stock position pays: 0.2% per interval on 200 USD is 0.4 USD, and three intervals
    // take cash, equity and realized PnL down by 1.2. A price-quoted position with no price has no
    // notional, so the accrual refuses rather than charging a rate to zero.
    {
        Portfolio financed(1, 1000.0);
        financed.apply(ExecutionReport{.side = exchange::Side::Buy,
                                       .exec_type = ExecType::Trade,
                                       .symbol = "STOCK",
                                       .price = 100.0,
                                       .quantity = 2,
                                       .owner = "test",
                                       .liquidity_role = LiquidityRole::Maker,
                                       .fee = 0.0});
        financed.mark_to_market({{"STOCK", 100.0}});

        for (int interval = 0; interval < 3; ++interval)
        {
            financed.accrue_interval_flow("STOCK", -0.002, 100.0);
        }
        assert(std::abs(financed.metrics().cash - 798.8) < 1e-9);
        assert(std::abs(financed.metrics().realized_pnl + 1.2) < 1e-9);
        assert(std::abs(financed.metrics().equity - 998.8) < 1e-9);

        assert(std::abs(financed.accrue_interval_flow("STOCK", -0.002, 0.0)) < 1e-12);
        assert(std::abs(financed.metrics().cash - 798.8) < 1e-9);
    }

    // The funding case on the fixed-face contract. Funding accrues on the contract's 10 USD face value
    // rather than on the BTC price, which is the same `unit_notional_at` value the fee and the cash
    // balance use. The published rate is the rate longs pay, so it enters as -0.0001, and one rate then
    // serves both sides: the long pays 0.002 USD per interval and the same rate pays the short 0.001.
    {
        Portfolio perp(1, 1000.0, 10.0);
        perp.apply(ExecutionReport{.side = exchange::Side::Buy,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "BTC-PERPETUAL",
                                   .price = 6000.0,
                                   .quantity = 2,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Maker,
                                   .fee = 0.0});
        perp.mark_to_market({{"BTC-PERPETUAL", 6000.0}});
        assert(std::abs(perp.metrics().equity - 1000.0) < 1e-9);

        double paid = 0.0;
        for (int interval = 0; interval < 3; ++interval)
        {
            paid += perp.accrue_interval_flow("BTC-PERPETUAL", -0.0001, 6000.0);
        }
        assert(std::abs(paid + 0.006) < 1e-9);
        assert(std::abs(perp.metrics().cash - 979.994) < 1e-9);
        assert(std::abs(perp.metrics().realized_pnl + 0.006) < 1e-9);
        assert(std::abs(perp.metrics().equity - 999.994) < 1e-9);

        // The face value does not move with the price, so doubling the mark leaves the flow unchanged.
        // A price-quoted instrument would have doubled it, which is what the stock case shows.
        assert(std::abs(perp.accrue_interval_flow("BTC-PERPETUAL", -0.0001, 12000.0) + 0.002) < 1e-9);
        assert(std::abs(perp.metrics().equity - 999.992) < 1e-9);

        // The short leg of the same market pays no new rate: the sign of the position is the only
        // difference, and it turns the long's payment into a receipt.
        Portfolio short_perp(1, 1000.0, 10.0);
        short_perp.apply(ExecutionReport{.side = exchange::Side::Sell,
                                         .exec_type = ExecType::Trade,
                                         .symbol = "BTC-PERPETUAL",
                                         .price = 6000.0,
                                         .quantity = 1,
                                         .owner = "test",
                                         .liquidity_role = LiquidityRole::Maker,
                                         .fee = 0.0});
        short_perp.mark_to_market({{"BTC-PERPETUAL", 6000.0}});
        assert(std::abs(short_perp.metrics().equity - 1000.0) < 1e-9);

        for (int interval = 0; interval < 3; ++interval)
        {
            short_perp.accrue_interval_flow("BTC-PERPETUAL", -0.0001, 6000.0);
        }
        assert(std::abs(short_perp.metrics().cash - 1010.003) < 1e-9);
        assert(std::abs(short_perp.metrics().realized_pnl - 0.003) < 1e-9);
        assert(std::abs(short_perp.metrics().equity - 1000.003) < 1e-9);
    }

    // One interval across the whole book, which is the shape a rate series has and the shape two legs
    // will need. Both legs enter the same published rate and their signs separate payer from receiver,
    // and a third position the rate map does not name accrues nothing rather than being guessed at.
    {
        Portfolio book(1, 1000.0, 10.0);
        book.apply(ExecutionReport{.side = exchange::Side::Buy,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "PERP-A",
                                   .price = 6000.0,
                                   .quantity = 2,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Maker,
                                   .fee = 0.0});
        book.apply(ExecutionReport{.side = exchange::Side::Sell,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "PERP-B",
                                   .price = 6000.0,
                                   .quantity = 1,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Maker,
                                   .fee = 0.0});
        book.apply(ExecutionReport{.side = exchange::Side::Buy,
                                   .exec_type = ExecType::Trade,
                                   .symbol = "PERP-C",
                                   .price = 6000.0,
                                   .quantity = 1,
                                   .owner = "test",
                                   .liquidity_role = LiquidityRole::Maker,
                                   .fee = 0.0});
        const std::unordered_map<std::string, double> marks{
            {"PERP-A", 6000.0}, {"PERP-B", 6000.0}, {"PERP-C", 6000.0}};
        const std::unordered_map<std::string, double> rates{{"PERP-A", -0.0001},
                                                            {"PERP-B", -0.0001}};
        book.mark_to_market(marks);
        // 20 USD long, 10 USD short and 10 USD long again: 20 USD of marked value on 1000 USD of cash.
        assert(std::abs(book.metrics().equity - 1000.0) < 1e-9);

        const double per_interval = book.accrue_interval_flows(rates, marks);
        // 0.002 paid by the long leg, 0.001 received by the short leg, nothing on PERP-C.
        assert(std::abs(per_interval + 0.001) < 1e-9);

        double total = per_interval;
        for (int interval = 1; interval < 5; ++interval)
        {
            total += book.accrue_interval_flows(rates, marks);
        }
        assert(std::abs(total + 0.005) < 1e-9);
        assert(std::abs(book.metrics().cash - 979.995) < 1e-9);
        assert(std::abs(book.metrics().realized_pnl + 0.005) < 1e-9);
        // PERP-C is untouched, so its 10 USD is still in equity: 979.995 cash plus 20 USD of net value.
        assert(std::abs(book.metrics().equity - 999.995) < 1e-9);
    }

    // A flat position and an unknown symbol both have no notional to charge a rate to, so neither
    // accrues and neither moves cash. The closed position is still in the map, which is why the guard
    // is on the quantity rather than on the lookup alone.
    {
        Portfolio closed(1, 1000.0, 10.0);
        closed.apply(ExecutionReport{.side = exchange::Side::Buy,
                                     .exec_type = ExecType::Trade,
                                     .symbol = "BTC-PERPETUAL",
                                     .price = 6000.0,
                                     .quantity = 1,
                                     .owner = "test",
                                     .liquidity_role = LiquidityRole::Maker,
                                     .fee = 0.0});
        closed.apply(ExecutionReport{.side = exchange::Side::Sell,
                                     .exec_type = ExecType::Trade,
                                     .symbol = "BTC-PERPETUAL",
                                     .price = 6000.0,
                                     .quantity = 1,
                                     .owner = "test",
                                     .liquidity_role = LiquidityRole::Maker,
                                     .fee = 0.0});
        assert(closed.find_position("BTC-PERPETUAL")->quantity == 0);
        assert(std::abs(closed.accrue_interval_flow("BTC-PERPETUAL", 0.5, 6000.0)) < 1e-12);
        assert(std::abs(closed.accrue_interval_flow("NOT-HELD", 0.5, 6000.0)) < 1e-12);
        assert(std::abs(closed.metrics().cash - 1000.0) < 1e-9);
        assert(std::abs(closed.metrics().equity - 1000.0) < 1e-9);
    }

    // A contract multiplier is what makes a listed future worth more than its index price, so it is the
    // difference between a micro E-mini position being 6100 USD and being 30500. One contract bought at 6100
    // with a multiplier of 5 therefore costs 30500 of the cash this cash-only model holds, and a single index
    // point of movement is 5 USD rather than 1.
    {
        Portfolio scaled(1, 100000.0, 0.0, 5.0);
        scaled.apply(ExecutionReport{.side = exchange::Side::Buy,
                                     .exec_type = ExecType::Trade,
                                     .symbol = "MESZ6",
                                     .price = 6100.0,
                                     .quantity = 1,
                                     .owner = "test",
                                     .liquidity_role = LiquidityRole::Maker,
                                     .fee = 0.62});
        assert(std::abs(scaled.metrics().cash - (100000.0 - 30500.0 - 0.62)) < 1e-6);
        scaled.mark_to_market({{"MESZ6", 6101.0}});
        assert(std::abs(scaled.metrics().unrealized_pnl - 5.0) < 1e-6);
        assert(std::abs(scaled.metrics().equity - (100000.0 - 0.62 + 5.0)) < 1e-6);

        // The same trade and the same one point move on an instrument whose unit is worth its price, which is
        // every instrument this repository traded before this one, and the field left at its default.
        Portfolio plain(1, 100000.0, 0.0);
        plain.apply(ExecutionReport{.side = exchange::Side::Buy,
                                    .exec_type = ExecType::Trade,
                                    .symbol = "SPY",
                                    .price = 6100.0,
                                    .quantity = 1,
                                    .owner = "test",
                                    .liquidity_role = LiquidityRole::Maker,
                                    .fee = 0.0});
        plain.mark_to_market({{"SPY", 6101.0}});
        assert(std::abs(plain.metrics().unrealized_pnl - 1.0) < 1e-6);
    }

    // The book's exposure, which one position cannot answer: gross is what fees are charged on and net is what
    // the position risk is. A single-leg book reports the same number twice, and that identity is what makes the
    // pair readable.
    {
        Portfolio two_legs(1, 1000.0);
        two_legs.apply(ExecutionReport{.side = exchange::Side::Buy,
                                       .exec_type = ExecType::Trade,
                                       .symbol = "LONG",
                                       .price = 100.0,
                                       .quantity = 10,
                                       .owner = "test",
                                       .liquidity_role = LiquidityRole::Maker,
                                       .fee = 0.10});
        two_legs.apply(ExecutionReport{.side = exchange::Side::Sell,
                                       .exec_type = ExecType::Trade,
                                       .symbol = "SHORT",
                                       .price = 50.0,
                                       .quantity = 20,
                                       .owner = "test",
                                       .liquidity_role = LiquidityRole::Maker,
                                       .fee = 0.20});
        const auto exposure = two_legs.exposure({{"LONG", 110.0}, {"SHORT", 50.0}});
        assert(exposure.symbol_count == 2);
        // Sorted by symbol, so the leg order is a property of the report rather than of the hash map.
        assert(exposure.legs[0].symbol == "LONG");
        assert(std::abs(exposure.legs[0].notional_usd - 1100.0) < 1e-9);
        assert(exposure.legs[1].symbol == "SHORT");
        assert(std::abs(exposure.legs[1].notional_usd + 1000.0) < 1e-9);
        // Gross is the sum of the sizes, net is the size of the sum: two legs that nearly cancel report a gross
        // of 2100 against a net of 100, which is the number a hedged book is judged by.
        assert(std::abs(exposure.gross_usd - 2100.0) < 1e-9);
        assert(std::abs(exposure.net_usd - 100.0) < 1e-9);
        assert(std::abs(exposure.gross_to_net() - 21.0) < 1e-9);
        assert(std::abs(exposure.fees_usd - 0.30) < 1e-12);

        // A closed leg is not an exposure, and a leg the run cannot price is still reported with its quantity
        // rather than dropped: an unpriceable position is a fact about the run, not something to hide.
        two_legs.apply(ExecutionReport{.side = exchange::Side::Sell,
                                       .exec_type = ExecType::Trade,
                                       .symbol = "LONG",
                                       .price = 120.0,
                                       .quantity = 10,
                                       .owner = "test",
                                       .liquidity_role = LiquidityRole::Taker,
                                       .fee = 0.0});
        const auto after_close = two_legs.exposure({{"LONG", 120.0}, {"SHORT", 50.0}});
        assert(after_close.symbol_count == 1);
        assert(after_close.legs[0].symbol == "SHORT");
        const auto unmarked = two_legs.exposure({});
        assert(unmarked.symbol_count == 1);
        assert(unmarked.legs[0].notional_usd == 0.0);
        assert(unmarked.gross_usd == 0.0);
        assert(unmarked.gross_to_net() == 0.0);
    }
}
