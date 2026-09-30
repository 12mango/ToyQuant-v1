#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "exchange/execution_report.h"
#include "strategy/strategy.h"

enum class L2SignalMode
{
    Baseline,
    Depth,
    Micro,
    Flow
};

struct L2MarketMakerConfig
{
    uint64_t order_size{1};
    double base_spread{1.0};
    int64_t inventory_limit{100};
    double tick_size{0.5};
    double imbalance_shift{1.0};
    double trade_imbalance_shift{1.0};
    L2SignalMode signal_mode{L2SignalMode::Depth};
    // Turns on the two flow guards. It sits next to the mode because it belongs to the signal choice,
    // and it used to be implied by `toxicity_flow_threshold <= 1.0`, a comparison that reads like
    // configuration and is not: the guards were off for every mode whose threshold sits above one, and
    // no setting of the config made that visible. The factory enables it for the flow strategies,
    // which is exactly the set that had it, so behaviour is unchanged and the switch is now readable.
    bool flow_guard{false};
    uint64_t trade_window{32};
    uint64_t refresh_price_ticks{1};
    uint64_t max_quote_age{20};
    double toxicity_flow_threshold{2.0};
    double weak_flow_threshold{0.9};
    double weak_flow_quote_scale{0.5};
    double weak_flow_spread_shift_ticks{1.0};
    bool quote_at_best_when_neutral{false};
    // How many quote cycles a stale quote is held while its own queue is being consumed, and the
    // inventory ratio the maker treats as neutral when quote_at_best_when_neutral is on.
    uint64_t max_queue_hold_count{3};
    double neutral_inventory_band{0.25};
    // The expected-value gate of the decision layer. A side is quoted only when the edge that fill would
    // capture, measured from the reference price in ticks, covers the maker fee plus this many extra ticks.
    // The fee in ticks is rate * price / tick_size, which is the same arithmetic the book's measurements
    // report: on the Deribit contract at 2020 prices that is 2.55 ticks, and it is the reason a quote at the
    // touch cannot pay for itself. A zero rate or zero extra ticks leaves the gate off, so every documented
    // run that does not ask for the gate keeps its numbers.
    double maker_fee_rate{0.0};
    double edge_cover_ticks{0.0};
    // A condition rather than a quote parameter: quote only when the top of book is at least this many ticks
    // wide. The bucketed markout is why. When the spread is one tick, the best a quote can be is the touch,
    // which sits half a tick from the midpoint, and those are the fills that resolve negative, so the rule
    // refuses the situation instead of the price. Zero leaves it off, as every recorded run asks.
    double min_spread_ticks{0.0};
    // Decision attribution: sides not quoted because the gate refused them.
    uint64_t edge_gate_skips{0};
    uint64_t min_spread_skips{0};
};

// The L2 market maker. Strategies that add behaviour to it derive from it rather than holding one:
// a wrapper has to hand-forward every callback, and a forgotten one changes behaviour silently.
class L2MarketMaker : public Strategy
{
   public:
    explicit L2MarketMaker(L2MarketMakerConfig config = {}) : config_(config) {}

    std::vector<StrategyOrder> on_top_of_book(const std::string& symbol,
                                              const TopOfBook& top) override
    {
        const double mid = (top.bid_price + top.ask_price) / 2.0;
        return quote(symbol, top, mid);
    }

    std::vector<StrategyOrder> on_l2_market_view(const L2MarketView& view) override
    {
        if (view.top.bid_price <= 0.0 || view.top.ask_price <= 0.0)
            return quote(view.symbol, view.top, 0.0);
        return quote(view.symbol, view.top, fair_price(view));
    }

    void on_market_trade(const MarketTrade& trade) override
    {
        if (trade.aggressor_side != Side::Buy && trade.aggressor_side != Side::Sell) return;
        recent_trades_.push_back({trade.aggressor_side, trade.quantity});
        if (trade.aggressor_side == Side::Buy)
            buy_volume_ += trade.quantity;
        else
            sell_volume_ += trade.quantity;
        while (recent_trades_.size() > config_.trade_window)
        {
            const auto stale_trade = recent_trades_.front();
            recent_trades_.pop_front();
            if (stale_trade.side == Side::Buy)
                buy_volume_ = buy_volume_ > stale_trade.quantity
                                   ? buy_volume_ - stale_trade.quantity
                                   : 0;
            else
                sell_volume_ = sell_volume_ > stale_trade.quantity
                                   ? sell_volume_ - stale_trade.quantity
                                   : 0;
        }
    }

    void on_queue_activity(Side side, uint64_t consumed_quantity) override
    {
        queue_activity_ += consumed_quantity;
        if (side == Side::Buy)
        {
            metrics_.buy_queue_consumed += consumed_quantity;
        }
        else if (side == Side::Sell)
        {
            metrics_.sell_queue_consumed += consumed_quantity;
        }
    }

    void on_order_submitted(const StrategyOrder& order) override
    {
        open_orders_[order.order_id] = order;
        metrics_.submitted_quantity += order.quantity;
        ++metrics_.quote_count;
        if (order.side == Side::Buy)
            ++metrics_.buy_quote_count;
        else if (order.side == Side::Sell)
            ++metrics_.sell_quote_count;
    }

    std::vector<uint64_t> cancel_requests() override
    {
        if (!cancel_pending_) return {};
        std::vector<uint64_t> order_ids;
        order_ids.reserve(open_orders_.size());
        for (const auto& entry : open_orders_) order_ids.push_back(entry.first);
        cancel_pending_ = false;
        metrics_.cancel_count += order_ids.size();
        return order_ids;
    }

    void request_cancel_all()
    {
        cancel_pending_ = !open_orders_.empty();
    }

    int64_t net_position() const override
    {
        return position_;
    }

    std::size_t working_order_count() const override
    {
        return open_orders_.size();
    }

    StrategyMetrics metrics() const override
    {
        auto result = metrics_;
        result.available = true;
        return result;
    }

    void on_order_update(const ExecutionReport& report) override
    {
        if (report.exec_type == ExecType::Cancelled || report.exec_type == ExecType::Filled)
        {
            open_orders_.erase(report.order_id);
            return;
        }
        if (report.exec_type != ExecType::Trade) return;

        auto order = open_orders_.find(report.order_id);
        if (order == open_orders_.end()) return;
        position_ += report.side == exchange::Side::Buy
                 ? static_cast<int64_t>(report.executed_quantity())
                 : -static_cast<int64_t>(report.executed_quantity());
        metrics_.filled_quantity += report.executed_quantity();
        ++metrics_.fill_count;
        metrics_.fees_paid += report.fee;
        if (report.side == exchange::Side::Buy)
            ++metrics_.buy_fill_count;
        else
            ++metrics_.sell_fill_count;
        order->second.quantity = order->second.quantity > report.executed_quantity()
                         ? order->second.quantity - report.executed_quantity()
                                     : 0;
        if (order->second.quantity == 0) open_orders_.erase(order);
    }

   private:
    double trade_imbalance() const
    {
        const uint64_t total_volume = buy_volume_ + sell_volume_;
        if (total_volume == 0) return 0.0;
        return static_cast<double>(buy_volume_) / static_cast<double>(total_volume) * 2.0 - 1.0;
    }

    // The mid, displaced by whichever signal this configuration selected. Each signal is a small
    // displacement of the same reference point, so the quoting logic below does not have to know
    // which one is active, and adding one no longer means editing the double-quote path.
    double fair_price(const L2MarketView& view) const
    {
        const double mid = (view.top.bid_price + view.top.ask_price) / 2.0;
        const double micro_price = view.micro_price > 0.0 ? view.micro_price : mid;
        switch (config_.signal_mode)
        {
            case L2SignalMode::Baseline:
                break;
            case L2SignalMode::Depth:
                return mid + view.depth_imbalance * config_.imbalance_shift;
            case L2SignalMode::Micro:
                return micro_price;
            case L2SignalMode::Flow:
                return micro_price + view.depth_imbalance * config_.imbalance_shift +
                       trade_imbalance() * config_.trade_imbalance_shift;
        }
        return mid;
    }

    // Position as a fraction of the inventory limit, clamped so a limit of zero cannot divide by it.
    double position_ratio() const
    {
        if (config_.inventory_limit <= 0) return 0.0;
        return std::clamp(static_cast<double>(position_) /
                              static_cast<double>(config_.inventory_limit),
                          -1.0, 1.0);
    }

    // Whether the resting quotes must be pulled before new ones go out, and which of the two reasons
    // it was. This is the whole risk decision of the maker: a quote that is never refreshed is a free
    // option, and one that is refreshed on every tick pays the queue again every tick.
    bool needs_refresh(const TopOfBook& top, double fair_price)
    {
        const double market_mid = (top.bid_price + top.ask_price) / 2.0;
        const bool market_moved = last_market_mid_ <= 0.0 ||
                                  std::abs(market_mid - last_market_mid_) >= config_.tick_size;
        if (market_moved) ++quote_age_;
        const bool price_changed =
            last_fair_price_ <= 0.0 ||
            std::abs(fair_price - last_fair_price_) >=
                static_cast<double>(config_.refresh_price_ticks) * config_.tick_size;
        // Holding a stale quote while the queue in front of it is being consumed is deliberate: that
        // consumption is the only mechanism that moves the order towards the front, so pulling the
        // quote to chase the price throws away the position it was accumulated for.
        if (quote_age_ >= config_.max_quote_age && queue_activity_ > 0 && !price_changed &&
            queue_hold_count_ < config_.max_queue_hold_count)
        {
            ++queue_hold_count_;
            quote_age_ = 0;
            queue_activity_ = 0;
            return false;
        }
        if (quote_age_ < config_.max_quote_age && !price_changed) return false;
        if (price_changed)
            ++metrics_.price_refresh_count;
        else
            ++metrics_.age_refresh_count;
        cancel_pending_ = true;
        last_market_mid_ = market_mid;
        return true;
    }

    std::vector<StrategyOrder> quote(const std::string& symbol, const TopOfBook& top,
                                     double fair_price)
    {
        std::vector<StrategyOrder> orders;
        if (top.bid_price <= 0.0 || top.ask_price <= 0.0 || top.bid_price >= top.ask_price ||
            config_.order_size == 0 || config_.tick_size <= 0.0)
        {
            cancel_pending_ = !open_orders_.empty();
            return orders;
        }

        if (!open_orders_.empty())
        {
            // needs_refresh records the reason and arms the cancel; the new quotes go out on the next
            // view, once the cancels have been acknowledged.
            needs_refresh(top, fair_price);
            return orders;
        }

        if (fair_price <= 0.0) fair_price = (top.bid_price + top.ask_price) / 2.0;

        const double inventory_ratio = position_ratio();
        const double inventory_shift = inventory_ratio * config_.base_spread;
        double weak_flow_spread_shift = 0.0;
        uint64_t buy_quantity = config_.order_size;
        uint64_t sell_quantity = config_.order_size;
        if (position_ >= config_.inventory_limit) buy_quantity = 0;
        if (position_ <= -config_.inventory_limit) sell_quantity = 0;
        if (config_.flow_guard)
        {
            const double flow = trade_imbalance();
            if (flow > config_.toxicity_flow_threshold) sell_quantity = 0;
            if (flow < -config_.toxicity_flow_threshold) buy_quantity = 0;
            if (std::abs(flow) > config_.weak_flow_threshold &&
                std::abs(inventory_ratio) > config_.neutral_inventory_band)
            {
                weak_flow_spread_shift = config_.weak_flow_spread_shift_ticks * config_.tick_size;
                const double weak_scale = std::max(0.0, config_.weak_flow_quote_scale);
                buy_quantity = static_cast<uint64_t>(std::max(
                    0.0, static_cast<double>(buy_quantity) * weak_scale));
                sell_quantity = static_cast<uint64_t>(std::max(
                    0.0, static_cast<double>(sell_quantity) * weak_scale));
            }
        }
        const double raw_bid_price = std::floor(
                                         (fair_price - config_.base_spread - inventory_shift -
                                          weak_flow_spread_shift) /
                                         config_.tick_size) *
                                     config_.tick_size;
        const double raw_ask_price = std::ceil(
                                         (fair_price + config_.base_spread - inventory_shift +
                                          weak_flow_spread_shift) /
                                         config_.tick_size) *
                                     config_.tick_size;
        const double passive_bid =
            std::floor(top.bid_price / config_.tick_size) * config_.tick_size;
        const double passive_ask =
            std::ceil(top.ask_price / config_.tick_size) * config_.tick_size;
        double bid_price = std::min(raw_bid_price, passive_bid);
        double ask_price = std::max(raw_ask_price, passive_ask);
        if (config_.quote_at_best_when_neutral && std::abs(inventory_ratio) < 0.25 &&
            std::abs(trade_imbalance()) < config_.toxicity_flow_threshold)
        {
            bid_price = passive_bid;
            ask_price = passive_ask;
        }
        if (!std::isfinite(bid_price) || !std::isfinite(ask_price) || bid_price >= ask_price)
            return orders;

        // The conditional rule, applied before the price gate because it is about the situation rather than
        // about the quote: a one tick book cannot offer anything but the touch, and the touch is the fill that
        // resolves negative. Refusing it is a decision, and it is counted so the refusal can be attributed.
        if (config_.min_spread_ticks > 0.0 &&
            (top.ask_price - top.bid_price) / config_.tick_size < config_.min_spread_ticks)
        {
            if (buy_quantity > 0 || sell_quantity > 0) ++config_.min_spread_skips;
            buy_quantity = 0;
            sell_quantity = 0;
        }

        // The expected-value gate, applied per side. The edge a fill would capture is the distance from the
        // reference price, and the cost it has to cover is the maker fee expressed in ticks at that price.
        const double reference_price = 0.5 * (top.bid_price + top.ask_price);
        const double fee_ticks = reference_price > 0.0 && config_.tick_size > 0.0
                                     ? config_.maker_fee_rate * reference_price / config_.tick_size
                                     : 0.0;
        const double required_edge_ticks = fee_ticks + config_.edge_cover_ticks;
        // A zero extra-tick setting leaves the gate off, which is what every recorded run asks for, so the
        // documented numbers do not move. A positive setting requires the quote to cover the fee plus it.
        if (config_.edge_cover_ticks > 0.0)
        {
            if ((reference_price - bid_price) / config_.tick_size < required_edge_ticks)
            {
                if (buy_quantity > 0) ++config_.edge_gate_skips;
                buy_quantity = 0;
            }
            if ((ask_price - reference_price) / config_.tick_size < required_edge_ticks)
            {
                if (sell_quantity > 0) ++config_.edge_gate_skips;
                sell_quantity = 0;
            }
        }

        if (buy_quantity > 0) orders.emplace_back(Side::Buy, symbol, bid_price, buy_quantity, 0);
        if (sell_quantity > 0) orders.emplace_back(Side::Sell, symbol, ask_price, sell_quantity, 0);
        if (!orders.empty())
        {
            last_fair_price_ = fair_price;
            last_market_mid_ = (top.bid_price + top.ask_price) / 2.0;
            quote_age_ = 0;
            queue_activity_ = 0;
            queue_hold_count_ = 0;
        }
        return orders;
    }

    struct TradeVolume
    {
        Side side;
        uint64_t quantity;
    };

    L2MarketMakerConfig config_;
    std::unordered_map<uint64_t, StrategyOrder> open_orders_;
    std::deque<TradeVolume> recent_trades_;
    uint64_t buy_volume_{0};
    uint64_t sell_volume_{0};
    int64_t position_{0};
    bool cancel_pending_{false};
    uint64_t quote_age_{0};
    double last_fair_price_{0.0};
    double last_market_mid_{0.0};
    uint64_t queue_activity_{0};
    uint64_t queue_hold_count_{0};
    StrategyMetrics metrics_;
};
