// The checks in this test are the test: keep them enabled even when the build defines
// NDEBUG, which is the case for the RelWithDebInfo profiling preset.
#ifdef NDEBUG
#undef NDEBUG
#endif

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
}
