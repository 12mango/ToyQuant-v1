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
}
