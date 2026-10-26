// The risk pipeline is tested here rather than through a replay because its contract is the order of the
// checks, and a replay cannot make two checks fail at once on purpose. Each block below isolates one
// control; the last block asserts the precedence the array declares.
#include <cassert>
#include <cstdint>

#include "app/risk_pipeline.h"

namespace
{
struct Fixture
{
    explicit Fixture(double mid = 100.0)
    {
        quote.bid_price = mid - 0.5;
        quote.ask_price = mid + 0.5;
    }

    RiskContext context(int64_t position = 0) const
    {
        RiskContext context;
        context.position = position;
        context.quantity_scale = 1;
        context.unit_notional_usd = 0.0;
        context.multiplier = 1.0;
        context.quote = &quote;
        context.now_seconds = 0.0;
        return context;
    }

    BboQuote quote{};
};

StrategyOrder make_order(Side side, double price, uint64_t quantity)
{
    return StrategyOrder(side, "TEST", price, quantity, 0);
}
}  // namespace

int main()
{
    Fixture fixture;

    {
        // Every control off: the pipeline must be the identity, which is what keeps the recorded runs.
        RiskPipeline risk;
        assert(risk.check(make_order(Side::Buy, 100.0, 10), fixture.context()).allowed);
        assert(risk.check(make_order(Side::Sell, 100.0, 10), fixture.context()).allowed);
        assert(risk.fat_finger_rejections() == 0);
        assert(risk.rate_limited_orders() == 0);
    }

    {
        RiskPipeline risk;
        risk.set_fat_finger_bps(10.0);
        assert(risk.check(make_order(Side::Buy, 100.09, 10), fixture.context()).allowed);
        auto decision = risk.check(make_order(Side::Buy, 100.11, 10), fixture.context());
        assert(!decision.allowed);
        assert(decision.reason == RiskPipeline::Reason::FatFinger);
        assert(risk.fat_finger_rejections() == 1);

        // The band is inclusive: exactly ten basis points is inside it.
        assert(risk.check(make_order(Side::Sell, 99.90, 10), fixture.context()).allowed);
        decision = risk.check(make_order(Side::Sell, 99.89, 10), fixture.context());
        assert(!decision.allowed);
        assert(decision.reason == RiskPipeline::Reason::FatFinger);
        assert(risk.fat_finger_rejections() == 2);

        // A run with no reference price cannot invent a band, so the order is allowed.
        RiskContext context = fixture.context();
        context.quote = nullptr;
        assert(risk.check(make_order(Side::Buy, 100.11, 10), context).allowed);
    }

    {
        RiskPipeline risk;
        risk.set_order_rate_limit(2.0, 2);
        RiskContext context = fixture.context();

        assert(risk.check(make_order(Side::Buy, 100.0, 1), context).allowed);
        assert(risk.check(make_order(Side::Buy, 100.0, 1), context).allowed);
        auto decision = risk.check(make_order(Side::Buy, 100.0, 1), context);
        assert(!decision.allowed);
        assert(decision.reason == RiskPipeline::Reason::RateLimit);
        assert(risk.rate_limited_orders() == 1);

        context.now_seconds = 0.5;  // one token at two orders a second
        assert(risk.check(make_order(Side::Buy, 100.0, 1), context).allowed);
        decision = risk.check(make_order(Side::Buy, 100.0, 1), context);
        assert(decision.reason == RiskPipeline::Reason::RateLimit);

        context.now_seconds = 1.1;  // 1.2 tokens, so one order and a fifth left
        assert(risk.check(make_order(Side::Buy, 100.0, 1), context).allowed);
        decision = risk.check(make_order(Side::Buy, 100.0, 1), context);
        assert(decision.reason == RiskPipeline::Reason::RateLimit);
    }

    {
        RiskPipeline risk;
        risk.set_hard_exposure_limit_usd(500.0);
        assert(risk.check(make_order(Side::Buy, 100.0, 5), fixture.context()).allowed);
        auto decision = risk.check(make_order(Side::Buy, 100.0, 6), fixture.context());
        assert(!decision.allowed);
        assert(decision.reason == RiskPipeline::Reason::Exposure);
        assert(risk.hard_limit_rejections() == 1);

        // The projection includes what is already resting, not only the fill so far.
        RiskContext context = fixture.context();
        context.pending_buy_quantity = 5;
        decision = risk.check(make_order(Side::Buy, 100.0, 1), context);
        assert(!decision.allowed);
        assert(decision.reason == RiskPipeline::Reason::Exposure);
    }

    {
        RiskPipeline risk;
        risk.set_position_limit(100);
        RiskContext context = fixture.context(90);
        assert(risk.check(make_order(Side::Buy, 100.0, 10), context).allowed);
        auto decision = risk.check(make_order(Side::Buy, 100.0, 11), context);
        assert(!decision.allowed);
        assert(decision.reason == RiskPipeline::Reason::Position);
        assert(risk.risk_rejected_orders() == 1);

        // The reducing side is always welcome; the gate is on the projected position, not on the side.
        assert(risk.check(make_order(Side::Sell, 100.0, 20), context).allowed);

        context.pending_buy_quantity = 5;
        decision = risk.check(make_order(Side::Buy, 100.0, 6), context);
        assert(!decision.allowed);
        assert(decision.reason == RiskPipeline::Reason::Position);
    }

    {
        // The switch is a latch, it keeps its first timestamp, and it runs before every other check.
        RiskPipeline risk;
        risk.set_kill_switch_loss_usd(50.0);
        risk.begin_cycle(-60.0, 12345);
        assert(risk.kill_switch_triggered());
        assert(risk.kill_switch_ts() == 12345);

        risk.set_fat_finger_bps(10.0);
        risk.set_order_rate_limit(1.0, 1);
        risk.set_hard_exposure_limit_usd(1.0);
        risk.set_position_limit(1);
        auto decision = risk.check(make_order(Side::Buy, 1000.0, 100), fixture.context());
        assert(!decision.allowed);
        assert(decision.reason == RiskPipeline::Reason::KillSwitch);
        assert(risk.kill_switch_rejected_orders() == 1);

        risk.begin_cycle(0.0, 99999);
        assert(risk.kill_switch_ts() == 12345);
        decision = risk.check(make_order(Side::Buy, 100.0, 1), fixture.context());
        assert(!decision.allowed);
        assert(decision.reason == RiskPipeline::Reason::KillSwitch);
        assert(risk.kill_switch_rejected_orders() == 2);
    }

    {
        // Fat finger before the bucket, the bucket before exposure, and exposure before position. The first
        // rejected order consumes neither a token nor any projectable exposure.
        RiskPipeline risk;
        risk.set_fat_finger_bps(10.0);
        risk.set_order_rate_limit(1.0, 1);
        risk.set_hard_exposure_limit_usd(100000.0);
        auto decision = risk.check(make_order(Side::Buy, 200.0, 1), fixture.context());
        assert(decision.reason == RiskPipeline::Reason::FatFinger);

        assert(risk.check(make_order(Side::Buy, 100.0, 1), fixture.context()).allowed);
        decision = risk.check(make_order(Side::Buy, 100.0, 1), fixture.context());
        assert(decision.reason == RiskPipeline::Reason::RateLimit);

        RiskPipeline exposure_first;
        exposure_first.set_hard_exposure_limit_usd(150.0);
        exposure_first.set_position_limit(1);
        decision = exposure_first.check(make_order(Side::Buy, 100.0, 2), fixture.context());
        assert(decision.reason == RiskPipeline::Reason::Exposure);
        assert(exposure_first.hard_limit_rejections() == 1);
        assert(exposure_first.risk_rejected_orders() == 0);

        exposure_first.set_hard_exposure_limit_usd(0.0);
        decision = exposure_first.check(make_order(Side::Buy, 100.0, 2), fixture.context());
        assert(decision.reason == RiskPipeline::Reason::Position);
    }

    {
        // A reducing order passes the bucket, the exposure cap and the position gate even when the resulting
        // position is still outside the limit: the controls refuse new risk, not the removal of it.
        RiskPipeline rate;
        rate.set_order_rate_limit(1.0, 1);
        RiskContext context = fixture.context(100);
        assert(rate.check(make_order(Side::Buy, 100.0, 10), context).allowed);
        assert(rate.check(make_order(Side::Buy, 100.0, 10), context).reason ==
               RiskPipeline::Reason::RateLimit);
        assert(rate.check(make_order(Side::Sell, 100.0, 10), context).allowed);
        assert(rate.check(make_order(Side::Buy, 100.0, 10), context).reason ==
               RiskPipeline::Reason::RateLimit);

        RiskPipeline exposure;
        exposure.set_hard_exposure_limit_usd(1000.0);
        RiskContext large = fixture.context(150);
        assert(exposure.check(make_order(Side::Sell, 100.0, 10), large).allowed);
        assert(exposure.check(make_order(Side::Buy, 100.0, 10), large).reason ==
               RiskPipeline::Reason::Exposure);

        RiskPipeline position;
        position.set_position_limit(100);
        assert(position.check(make_order(Side::Sell, 100.0, 10), large).allowed);
        assert(position.check(make_order(Side::Buy, 100.0, 10), large).reason ==
               RiskPipeline::Reason::Position);
    }

    return 0;
}
