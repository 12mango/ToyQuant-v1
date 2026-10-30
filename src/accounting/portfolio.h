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
    // Quantity traded in this symbol, both directions counted, which is not recoverable from the position:
    // a buy and a sell of the same size leave `quantity` where it was. It exists for reconciliation,
    // where a lost fill that happened to net out is still a lost fill, and it is a plain count of what
    // was applied rather than a second opinion about the position.
    uint64_t traded_quantity{0};
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
    // `multiplier` mirrors InstrumentSpec::multiplier: the USD value of one price unit in one quantity
    // unit, which is 5 for a micro E-mini and 1 for everything whose unit value is just its price.
    explicit Portfolio(uint64_t quantity_scale = 1, double initial_cash = 1000.0,
                       double unit_notional_usd = 0.0, double multiplier = 1.0)
        : quantity_scale_(quantity_scale == 0 ? 1 : quantity_scale),
          unit_notional_usd_(unit_notional_usd),
          multiplier_(multiplier),
          initial_cash_(initial_cash),
          cash_(initial_cash),
          // Matches reset(). Leaving equity at its default of zero made a summary that never
          // marks to market report equity as zero while cash was correct.
          equity_(initial_cash)
    {
    }

    void apply(const ExecutionReport& report);
    void mark_to_market(const std::unordered_map<std::string, double>& prices);
    // The same valuation as `mark_to_market`, read instead of written. The equity series a run samples
    // needs the marked account at an arbitrary moment, and it cannot use `mark_to_market` for that: that
    // method is not const, and it overwrites the unrealized PnL and equity the kill switch reads in the
    // middle of an event. Both go through one loop, so the sampled number and the reported one are the
    // same arithmetic rather than two implementations that agree today.
    double equity_at(const std::unordered_map<std::string, double>& prices) const;
    void reset();

    // A fee that belongs to no single execution: a schedule's floor under one order's commission is charged
    // when the order ends, because that is the only moment the order's total is known. It is booked to fees
    // and to cash exactly like the fee of a fill, and deliberately not to the maker or taker split, which
    // counts the venue's own fees per execution and would be reporting a commission as a venue charge.
    void charge_fee(double fee);

    // One interval of a per-interval cash flow on one position: the funding a perpetual pays, the basis
    // a future pays at roll, a dividend, or the financing a position is charged. One method for all four
    // because the ledger side is identical and only the source of the rate differs.
    //
    // `interval_rate` is the rate one USD of notional earns per interval to a long position, so the
    // position's own sign decides the direction of the payment: the flow is the rate times the signed
    // notional. One rate therefore serves both sides of the same market, and a short pays exactly what
    // a long receives. A published funding rate is the rate longs pay, so it enters as `-funding_rate`;
    // a dividend is positive, financing is negative, and the basis a future pays at roll is negative on
    // the long leg.
    //
    // The notional is the USD value this class already assigns the position at `mark_price`
    // (unit_notional_at): a price-quoted unit accrues on `quantity * mark_price` and a fixed-USD-face
    // contract on its face value, so the entry price and unrealized PnL play no part.
    //
    // Cash, realized PnL and equity move by the returned amount, because an accrued flow has been paid
    // rather than marked: unlike a price move it does not wait for a closing trade. Equity tracks cash
    // so the identity mark_to_market produced still holds between marks.
    //
    // Returns the signed USD amount credited to cash. A flat position, an unknown symbol, or a
    // non-positive mark price on a price-quoted instrument accrues nothing, because none of them has a
    // notional to charge a rate to.
    double accrue_interval_flow(const std::string& symbol, double interval_rate, double mark_price);

    // One interval across the whole book, which is the shape a rate series has and the shape two legs
    // will need: a rate per symbol and a mark per symbol. A position that either map does not name
    // accrues nothing rather than being guessed at, so a leg that pays no carry, or a symbol with no
    // price, is simply absent. Returns the summed amount.
    double accrue_interval_flows(const std::unordered_map<std::string, double>& interval_rates,
                                 const std::unordered_map<std::string, double>& mark_prices);

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
    // The one valuation loop. `mark_to_market` writes its two outputs into the account, `equity_at`
    // reads only the equity, and because the arithmetic is the same the sampled curve and the final
    // [PORTFOLIO] line cannot drift apart.
    void mark_values(const std::unordered_map<std::string, double>& prices, double& unrealized_pnl,
                     double& equity) const;

    double units(uint64_t quantity) const
    {
        return static_cast<double>(quantity) / static_cast<double>(quantity_scale_);
    }

    // USD value of one quantity unit at `price`.
    double unit_value(double price) const
    {
        return unit_notional_at(unit_notional_usd_, multiplier_, price);
    }

    // USD value of `amount` units held from `entry_price` to `price`.
    double value_of(double amount, double entry_price, double price) const
    {
        return position_value(unit_notional_usd_, multiplier_, amount, entry_price, price);
    }

    uint64_t quantity_scale_;
    double unit_notional_usd_;
    double multiplier_;
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
