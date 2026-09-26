#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "strategy/l2_market_maker.h"

struct ActiveL2MarketMakerConfig
{
    L2MarketMakerConfig base;
    double volatility_alpha{0.25};
    double pause_after_ticks{9.0};
    uint64_t warmup_trades{0};
    uint64_t warmup_views{0};
};

class ActiveL2MarketMaker final : public Strategy
{
   public:
    explicit ActiveL2MarketMaker(ActiveL2MarketMakerConfig config)
        : config_(config), base_(config.base)
    {
    }

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& top) override
    {
        update_volatility(top);
        if (should_pause()) return pause();
        return base_.on_top_of_book(symbol, top);
    }

    std::vector<StrategyOrder> on_l2_market_view(const L2MarketView& view) override
    {
        update_volatility(view.top, view.ts);
        if (view.top.bid_price > 0.0 && view.top.ask_price > 0.0) ++valid_views_;
        if (!warmup_complete()) return {};
        if (should_pause()) return pause();
        return base_.on_l2_market_view(view);
    }

    void on_market_trade(const MarketTrade& trade) override
    {
        base_.on_market_trade(trade);
        ++market_trades_;
    }

    void on_queue_activity(Side side, uint64_t consumed_quantity) override
    {
        base_.on_queue_activity(side, consumed_quantity);
    }

    void on_order_submitted(const StrategyOrder& order) override
    {
        base_.on_order_submitted(order);
    }

    std::vector<uint64_t> cancel_requests() override
    {
        return base_.cancel_requests();
    }

    int64_t net_position() const override
    {
        return base_.net_position();
    }

    std::size_t working_order_count() const override
    {
        return base_.working_order_count();
    }

    StrategyMetrics metrics() const override
    {
        auto result = base_.metrics();
        result.risk_pause_count = risk_pause_count_;
        return result;
    }

    void on_order_update(const ExecutionReport& report) override
    {
        base_.on_order_update(report);
    }

    double volatility_ticks() const
    {
        return volatility_ticks_;
    }

   private:
    bool should_pause() const
    {
        return volatility_ticks_ >= config_.pause_after_ticks;
    }

    bool warmup_complete() const
    {
        return market_trades_ >= config_.warmup_trades && valid_views_ >= config_.warmup_views;
    }

    std::vector<StrategyOrder> pause()
    {
        if (base_.working_order_count() > 0) ++risk_pause_count_;
        base_.request_cancel_all();
        return {};
    }

    void update_volatility(const TopOfBook& top, uint64_t ts = 0)
    {
        if (top.bid_price <= 0.0 || top.ask_price <= 0.0 || config_.base.tick_size <= 0.0)
            return;

        const double mid = (top.bid_price + top.ask_price) / 2.0;
        if (last_mid_ > 0.0)
        {
            const double move_ticks = std::abs(mid - last_mid_) / config_.base.tick_size;
            double normalized_move_ticks = move_ticks;
            if (ts > 0 && last_ts_ > 0 && ts > last_ts_)
            {
                const double elapsed_seconds =
                    static_cast<double>(ts - last_ts_) / 1000000.0;
                constexpr double min_elapsed_seconds = 0.05;
                normalized_move_ticks =
                    move_ticks / std::max(elapsed_seconds, min_elapsed_seconds);
            }
            volatility_ticks_ = config_.volatility_alpha * normalized_move_ticks +
                                (1.0 - config_.volatility_alpha) * volatility_ticks_;
        }
        last_mid_ = mid;
        last_ts_ = ts;
    }

    ActiveL2MarketMakerConfig config_;
    L2MarketMaker base_;
    double last_mid_{0.0};
    uint64_t last_ts_{0};
    double volatility_ticks_{0.0};
    uint64_t risk_pause_count_{0};
    uint64_t market_trades_{0};
    uint64_t valid_views_{0};
};
