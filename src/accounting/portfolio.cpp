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
    fees_paid_ += report.fee;
    if (report.liquidity_role == LiquidityRole::Maker)
    {
        maker_fees_ += report.fee;
        ++maker_trade_count_;
    }
    else if (report.liquidity_role == LiquidityRole::Taker)
    {
        taker_fees_ += report.fee;
        ++taker_trade_count_;
    }
    cash_ -= report.fee;
    realized_pnl_ -= report.fee;

    // A trade can close part of an existing position and open the rest. The closing portion is
    // valued from the position's entry price and the opening portion at the trade price, so the
    // two are settled separately. For a spot style instrument the closing value reduces to
    // quantity * trade price, which is the previous behaviour.
    const auto settle_close = [&](int64_t close_quantity)
    {
        const double closed = units(static_cast<uint64_t>(close_quantity));
        const double value_now = value_of(closed, position.average_price, report.price);
        const double value_at_entry = unit_value(position.average_price) * closed;
        if (report.side == exchange::Side::Buy)
        {
            // Buying back a short: pay the current value and realise entry minus current.
            cash_ -= value_now;
            realized_pnl_ += value_at_entry - value_now;
        }
        else
        {
            // Selling a long: receive the current value.
            cash_ += value_now;
            realized_pnl_ += value_now - value_at_entry;
        }
    };

    int64_t remaining_quantity = static_cast<int64_t>(report.executed_quantity());
    if (report.side == exchange::Side::Buy)
    {
        if (position.quantity < 0)
        {
            const int64_t close_quantity = std::min(-position.quantity, remaining_quantity);
            settle_close(close_quantity);
            position.quantity += close_quantity;
            remaining_quantity -= close_quantity;
        }

        if (remaining_quantity > 0)
        {
            const double open = units(static_cast<uint64_t>(remaining_quantity));
            const double existing =
                units(static_cast<uint64_t>(std::max<int64_t>(0, position.quantity)));
            const double total = existing + open;
            position.average_price =
                total > 0.0 ? (existing * position.average_price + open * report.price) / total
                            : 0.0;
            position.quantity += remaining_quantity;
            cash_ -= unit_value(report.price) * open;
        }
    }
    else
    {
        if (position.quantity > 0)
        {
            const int64_t close_quantity = std::min(position.quantity, remaining_quantity);
            settle_close(close_quantity);
            position.quantity -= close_quantity;
            remaining_quantity -= close_quantity;
        }

        if (remaining_quantity > 0)
        {
            const double open = units(static_cast<uint64_t>(remaining_quantity));
            const double existing =
                units(static_cast<uint64_t>(std::max<int64_t>(0, -position.quantity)));
            const double total = existing + open;
            position.average_price =
                total > 0.0 ? (existing * position.average_price + open * report.price) / total
                            : 0.0;
            position.quantity -= remaining_quantity;
            cash_ += unit_value(report.price) * open;
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

        const double amount = units(static_cast<uint64_t>(std::llabs(position.quantity)));
        const double value_now = value_of(amount, position.average_price, price_it->second);
        const double value_at_entry = unit_value(position.average_price) * amount;
        if (position.quantity > 0)
        {
            unrealized_pnl_ += value_now - value_at_entry;
            equity_ += value_now;
        }
        else
        {
            unrealized_pnl_ += value_at_entry - value_now;
            equity_ -= value_now;
        }
    }
}

const PortfolioPosition* Portfolio::find_position(const std::string& symbol) const
{
    const auto it = positions_.find(symbol);
    return it == positions_.end() ? nullptr : &it->second;
}
