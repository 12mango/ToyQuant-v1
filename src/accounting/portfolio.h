#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>

#include "common/instrument_spec.h"
#include "exchange/execution_report.h"

struct PortfolioPosition
{
    int64_t quantity{0};
    double average_price{0.0};
};

struct PortfolioMetrics
{
    double cash{0.0};
    double realized_pnl{0.0};
    double unrealized_pnl{0.0};
    double equity{0.0};
    double fees_paid{0.0};
    double maker_fees{0.0};
    double taker_fees{0.0};
    uint64_t maker_trade_count{0};
    uint64_t taker_trade_count{0};

    std::string to_log_string() const
    {
        std::ostringstream stream;
        stream << "cash=" << cash << " realized_pnl=" << realized_pnl
               << " unrealized_pnl=" << unrealized_pnl << " equity=" << equity
               << " fees_paid=" << fees_paid << " maker_fees=" << maker_fees
               << " taker_fees=" << taker_fees << " maker_trade_count=" << maker_trade_count
               << " taker_trade_count=" << taker_trade_count;
        return stream.str();
    }
};

class Portfolio
{
   public:
    // `unit_notional_usd` mirrors InstrumentSpec::unit_notional_usd: 0 means the traded price is
    // the USD value of one quantity unit, which is the spot case. A fixed value marks an
    // instrument whose contracts have a set USD face value.
    explicit Portfolio(uint64_t quantity_scale = 1, double initial_cash = 1000.0,
                       double unit_notional_usd = 0.0)
        : quantity_scale_(quantity_scale == 0 ? 1 : quantity_scale),
          unit_notional_usd_(unit_notional_usd),
          initial_cash_(initial_cash),
          cash_(initial_cash)
    {
    }

    void apply(const ExecutionReport& report);
    void mark_to_market(const std::unordered_map<std::string, double>& prices);
    void reset();

    const std::unordered_map<std::string, PortfolioPosition>& positions() const
    {
        return positions_;
    }

    PortfolioMetrics metrics() const
    {
        return {cash_,       realized_pnl_, unrealized_pnl_,    equity_,           fees_paid_,
                maker_fees_, taker_fees_,   maker_trade_count_, taker_trade_count_};
    }

    const PortfolioPosition* find_position(const std::string& symbol) const;

   private:
    double units(uint64_t quantity) const
    {
        return static_cast<double>(quantity) / static_cast<double>(quantity_scale_);
    }

    // USD value of one quantity unit at `price`.
    double unit_value(double price) const
    {
        return unit_notional_at(unit_notional_usd_, price);
    }

    // USD value of `amount` units held from `entry_price` to `price`.
    double value_of(double amount, double entry_price, double price) const
    {
        return position_value(unit_notional_usd_, amount, entry_price, price);
    }

    uint64_t quantity_scale_;
    double unit_notional_usd_;
    double initial_cash_;
    double cash_;
    double realized_pnl_{0.0};
    double unrealized_pnl_{0.0};
    double equity_{0.0};
    double fees_paid_{0.0};
    double maker_fees_{0.0};
    double taker_fees_{0.0};
    uint64_t maker_trade_count_{0};
    uint64_t taker_trade_count_{0};
    std::unordered_map<std::string, PortfolioPosition> positions_;
};
