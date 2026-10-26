#include "risk_pipeline.h"

#include <algorithm>
#include <cmath>

#include "common/instrument_spec.h"

void RiskPipeline::set_position_limit(int64_t limit)
{
    limits_.position_limit = limit;
}

void RiskPipeline::set_hard_exposure_limit_usd(double limit)
{
    limits_.max_exposure_usd = limit;
}

void RiskPipeline::set_kill_switch_loss_usd(double loss)
{
    limits_.kill_switch_loss_usd = loss;
}

void RiskPipeline::set_fat_finger_bps(double bps)
{
    limits_.fat_finger_bps = bps;
}

void RiskPipeline::set_order_rate_limit(double orders_per_second, uint64_t burst)
{
    limits_.max_orders_per_second = orders_per_second;
    limits_.order_burst = burst == 0 ? 1 : burst;
    // A new limit starts with a full bucket. The alternative would make a run's first order depend on how
    // much time passed before the configuration was applied, which is not a property of the strategy.
    bucket_initialized_ = false;
}

void RiskPipeline::begin_cycle(double net_pnl, uint64_t ts_us)
{
    if (limits_.kill_switch_loss_usd <= 0.0 || kill_switch_triggered_) return;
    if (net_pnl <= -limits_.kill_switch_loss_usd)
    {
        kill_switch_triggered_ = true;
        kill_switch_ts_ = ts_us;
    }
}

RiskPipeline::Decision RiskPipeline::check(const StrategyOrder& order, const RiskContext& context)
{
    for (const Check check : checks_)
    {
        const Decision decision = (this->*check)(order, context);
        if (!decision.allowed) return decision;
    }
    return {};
}

RiskPipeline::Decision RiskPipeline::check_kill_switch(const StrategyOrder& order,
                                                       const RiskContext& context)
{
    (void)order;
    (void)context;
    if (!kill_switch_triggered_) return {};
    ++kill_switch_rejected_orders_;
    return {false, Reason::KillSwitch};
}

RiskPipeline::Decision RiskPipeline::check_fat_finger(const StrategyOrder& order,
                                                      const RiskContext& context)
{
    if (limits_.fat_finger_bps <= 0.0 || order.quantity == 0 || order.price <= 0.0) return {};
    if (context.quote == nullptr || context.quote->bid_price <= 0.0 ||
        context.quote->ask_price <= 0.0)
        return {};

    const double mid = (context.quote->bid_price + context.quote->ask_price) / 2.0;
    if (mid <= 0.0) return {};
    const double deviation_bps = std::abs(order.price - mid) / mid * 10000.0;
    if (deviation_bps <= limits_.fat_finger_bps) return {};

    ++fat_finger_rejections_;
    return {false, Reason::FatFinger};
}

RiskPipeline::Decision RiskPipeline::check_rate_limit(const StrategyOrder& order,
                                                      const RiskContext& context)
{
    if (limits_.max_orders_per_second <= 0.0 || order.quantity == 0) return {};

    const double burst = std::max(1.0, static_cast<double>(limits_.order_burst));
    const double now = context.now_seconds;
    if (!bucket_initialized_)
    {
        tokens_ = burst;
        last_token_time_ = now;
        bucket_initialized_ = true;
    }
    else if (now > last_token_time_)
    {
        tokens_ = std::min(burst, tokens_ + (now - last_token_time_) * limits_.max_orders_per_second);
        last_token_time_ = now;
    }

    if (tokens_ + 1e-12 >= 1.0)
    {
        tokens_ = std::max(0.0, tokens_ - 1.0);
        return {};
    }

    ++rate_limited_orders_;
    return {false, Reason::RateLimit};
}

RiskPipeline::Decision RiskPipeline::check_exposure(const StrategyOrder& order,
                                                    const RiskContext& context)
{
    if (limits_.max_exposure_usd <= 0.0 || order.quantity == 0) return {};

    const int64_t signed_quantity = order.side == Side::Buy
                                        ? static_cast<int64_t>(order.quantity)
                                        : -static_cast<int64_t>(order.quantity);
    const int64_t projected = context.position + context.pending_buy_quantity -
                              context.pending_sell_quantity + signed_quantity;
    const double scale = static_cast<double>(std::max<uint64_t>(1, context.quantity_scale));
    const double marked = std::abs(static_cast<double>(projected)) / scale *
                          unit_notional_at(context.unit_notional_usd, context.multiplier,
                                           order.price);
    if (marked <= limits_.max_exposure_usd) return {};

    ++hard_limit_rejections_;
    return {false, Reason::Exposure};
}

RiskPipeline::Decision RiskPipeline::check_position(const StrategyOrder& order,
                                                    const RiskContext& context)
{
    if (limits_.position_limit <= 0 || order.quantity == 0) return {};

    const int64_t signed_quantity = order.side == Side::Buy
                                        ? static_cast<int64_t>(order.quantity)
                                        : -static_cast<int64_t>(order.quantity);
    const int64_t projected = context.position + context.pending_buy_quantity -
                              context.pending_sell_quantity + signed_quantity;
    if (projected <= limits_.position_limit && projected >= -limits_.position_limit) return {};

    ++risk_rejected_orders_;
    return {false, Reason::Position};
}
