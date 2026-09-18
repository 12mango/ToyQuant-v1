#include "portfolio.h"

#include <algorithm>
#include <cstdlib>

void Portfolio::reset()
{
    positions_.clear();
    cash_ = initial_cash_;
    realized_pnl_ = 0.0;
    unrealized_pnl_ = 0.0;
    equity_ = cash_;
    fees_paid_ = 0.0;
    maker_fees_ = 0.0;
    taker_fees_ = 0.0;
    maker_trade_count_ = 0;
    taker_trade_count_ = 0;
}

void Portfolio::apply(const ExecutionReport& report)
{
    if (report.exec_type != ExecType::Trade || report.quantity == 0) return;

    auto& position = positions_[report.symbol];
    const double quantity = real_quantity(report.quantity);
    const double notional = report.price * quantity;
    const double fee = report.fee;
    fees_paid_ += fee;
    if (report.liquidity_role == LiquidityRole::Maker)
    {
        maker_fees_ += fee;
        ++maker_trade_count_;
    }
    else if (report.liquidity_role == LiquidityRole::Taker)
    {
        taker_fees_ += fee;
        ++taker_trade_count_;
    }
    cash_ += report.side == exchange::Side::Buy ? -notional - fee : notional - fee;
    realized_pnl_ -= fee;

    int64_t remaining_quantity = static_cast<int64_t>(report.quantity);
    if (report.side == exchange::Side::Buy)
    {
        if (position.quantity < 0)
        {
            const int64_t close_quantity = std::min(-position.quantity, remaining_quantity);
            const double closed = real_quantity(static_cast<uint64_t>(close_quantity));
            realized_pnl_ += closed * (position.average_price - report.price);
            position.quantity += close_quantity;
            remaining_quantity -= close_quantity;
        }

        if (remaining_quantity > 0)
        {
            const double open = real_quantity(static_cast<uint64_t>(remaining_quantity));
            const double existing =
                real_quantity(static_cast<uint64_t>(std::max<int64_t>(0, position.quantity)));
            const double total = existing + open;
            position.average_price =
                total > 0.0 ? (existing * position.average_price + open * report.price) / total
                            : 0.0;
            position.quantity += remaining_quantity;
        }
    }
    else
    {
        if (position.quantity > 0)
        {
            const int64_t close_quantity = std::min(position.quantity, remaining_quantity);
            const double closed = real_quantity(static_cast<uint64_t>(close_quantity));
            realized_pnl_ += closed * (report.price - position.average_price);
            position.quantity -= close_quantity;
            remaining_quantity -= close_quantity;
        }

        if (remaining_quantity > 0)
        {
            const double open = real_quantity(static_cast<uint64_t>(remaining_quantity));
            const double existing =
                real_quantity(static_cast<uint64_t>(std::max<int64_t>(0, -position.quantity)));
            const double total = existing + open;
            position.average_price =
                total > 0.0 ? (existing * position.average_price + open * report.price) / total
                            : 0.0;
            position.quantity -= remaining_quantity;
        }
    }
}

void Portfolio::mark_to_market(const std::unordered_map<std::string, double>& prices)
{
    unrealized_pnl_ = 0.0;
    equity_ = cash_;
    for (const auto& [symbol, position] : positions_)
    {
        const auto price_it = prices.find(symbol);
        if (price_it == prices.end() || position.quantity == 0) continue;
        const double quantity = real_quantity(static_cast<uint64_t>(std::llabs(position.quantity)));
        const double pnl = position.quantity > 0
                               ? quantity * (price_it->second - position.average_price)
                               : quantity * (position.average_price - price_it->second);
        unrealized_pnl_ += pnl;
        equity_ += static_cast<double>(position.quantity) / static_cast<double>(quantity_scale_) *
                   price_it->second;
    }
}

const PortfolioPosition* Portfolio::find_position(const std::string& symbol) const
{
    const auto it = positions_.find(symbol);
    return it == positions_.end() ? nullptr : &it->second;
}
