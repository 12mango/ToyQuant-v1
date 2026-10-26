#pragma once

#include <array>
#include <cstdint>

#include "common/market_event.h"
#include "strategy/strategy.h"

// The pre-trade controls, in one place, in the order they run. They used to live inside
// `Pipeline::submit_strategy_actions`, where the only way to test one was to send orders through the engine,
// and where a fifth control would have been another branch in a loop that already mixed the switch, the
// exposure cap and the position gate with order submission.
//
// The order is the policy and it is deliberately data rather than nesting:
//
//   1. the kill switch, a latch that stops everything once the account's loss reaches its limit;
//   2. the fat-finger band, which refuses a price too far from the current midpoint before any other check
//      spends a rate token or reserves exposure;
//   3. the order rate, a token bucket, because a strategy that has gone wrong usually goes wrong quickly;
//   4. the hard exposure cap, in USD, on the projected position including resting orders;
//   5. the position gate, in quantity units, on the same projection.
//
// A rejected order consumes nothing: it does not take a rate token and it does not reserve pending quantity.
// A control at zero is off, so a run that names none of them behaves exactly as it did before this class
// existed, which is the same rule the decision-layer gates follow.

struct RiskLimits
{
    int64_t position_limit{0};
    double max_exposure_usd{0.0};
    double kill_switch_loss_usd{0.0};
    // Reject an order whose price is more than this many basis points from the current midpoint. 0 is off.
    double fat_finger_bps{0.0};
    // Orders per second and the bucket's size. A rate of 0 is off; the burst defaults to one order.
    double max_orders_per_second{0.0};
    uint64_t order_burst{1};
};

// What a check needs to know about the account and the order's symbol at the moment of the decision. The
// pipeline owns the position and the pending quantities, so they arrive by value rather than being read back
// from the portfolio, which would make a check depend on when the last report was booked.
struct RiskContext
{
    int64_t position{0};
    int64_t pending_buy_quantity{0};
    int64_t pending_sell_quantity{0};
    uint64_t quantity_scale{1};
    double unit_notional_usd{0.0};
    double multiplier{1.0};
    // The latest BBO for the order's symbol, or null when the feed has not produced one. A fat-finger check
    // without a reference price cannot say anything, so it allows the order rather than inventing a band.
    const BboQuote* quote{nullptr};
    // The event clock in seconds. The bucket uses it, and a clock that goes backwards (an out-of-order
    // replay) refills nothing rather than granting orders.
    double now_seconds{0.0};
};

class RiskPipeline
{
   public:
    enum class Reason
    {
        None,
        KillSwitch,
        FatFinger,
        RateLimit,
        Exposure,
        Position
    };

    struct Decision
    {
        bool allowed{true};
        Reason reason{Reason::None};
    };

    RiskPipeline() = default;
    explicit RiskPipeline(RiskLimits limits) : limits_(limits) {}

    void set_position_limit(int64_t limit);
    void set_hard_exposure_limit_usd(double limit);
    void set_kill_switch_loss_usd(double loss);
    void set_fat_finger_bps(double bps);
    void set_order_rate_limit(double orders_per_second, uint64_t burst);

    // Called once per decision cycle, before the orders that cycle produced are checked. The switch is a
    // latch: it trips on the account's realised plus unrealised result and stays tripped, and the timestamp
    // is the event clock of the cycle that tripped it, not the clock of the order that was refused.
    void begin_cycle(double net_pnl, uint64_t ts_us);

    Decision check(const StrategyOrder& order, const RiskContext& context);

    int64_t position_limit() const
    {
        return limits_.position_limit;
    }

    double max_exposure_limit_usd() const
    {
        return limits_.max_exposure_usd;
    }

    double kill_switch_loss_usd() const
    {
        return limits_.kill_switch_loss_usd;
    }

    bool fat_finger_enabled() const
    {
        return limits_.fat_finger_bps > 0.0;
    }

    bool rate_limit_enabled() const
    {
        return limits_.max_orders_per_second > 0.0;
    }

    double fat_finger_bps() const
    {
        return limits_.fat_finger_bps;
    }

    double max_orders_per_second() const
    {
        return limits_.max_orders_per_second;
    }

    uint64_t order_burst() const
    {
        return limits_.order_burst;
    }

    bool kill_switch_triggered() const
    {
        return kill_switch_triggered_;
    }

    uint64_t kill_switch_ts() const
    {
        return kill_switch_ts_;
    }

    uint64_t kill_switch_rejected_orders() const
    {
        return kill_switch_rejected_orders_;
    }

    uint64_t hard_limit_rejections() const
    {
        return hard_limit_rejections_;
    }

    uint64_t risk_rejected_orders() const
    {
        return risk_rejected_orders_;
    }

    uint64_t fat_finger_rejections() const
    {
        return fat_finger_rejections_;
    }

    uint64_t rate_limited_orders() const
    {
        return rate_limited_orders_;
    }

   private:
    using Check = Decision (RiskPipeline::*)(const StrategyOrder&, const RiskContext&);

    Decision check_kill_switch(const StrategyOrder& order, const RiskContext& context);
    Decision check_fat_finger(const StrategyOrder& order, const RiskContext& context);
    Decision check_rate_limit(const StrategyOrder& order, const RiskContext& context);
    Decision check_exposure(const StrategyOrder& order, const RiskContext& context);
    Decision check_position(const StrategyOrder& order, const RiskContext& context);

    RiskLimits limits_;
    bool kill_switch_triggered_{false};
    uint64_t kill_switch_ts_{0};
    uint64_t kill_switch_rejected_orders_{0};
    uint64_t hard_limit_rejections_{0};
    uint64_t risk_rejected_orders_{0};
    uint64_t fat_finger_rejections_{0};
    uint64_t rate_limited_orders_{0};
    // Token bucket state. `tokens_` starts full on the first order, which is what makes a burst of N
    // possible at the first event rather than after N seconds of waiting.
    double tokens_{0.0};
    double last_token_time_{0.0};
    bool bucket_initialized_{false};

    const std::array<Check, 5> checks_{
        &RiskPipeline::check_kill_switch,
        &RiskPipeline::check_fat_finger,
        &RiskPipeline::check_rate_limit,
        &RiskPipeline::check_exposure,
        &RiskPipeline::check_position};
};
