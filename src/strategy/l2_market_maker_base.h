#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "exchange/execution_report.h"
#include "strategy/strategy.h"

// The root of the L2 family. It holds the whole quoting engine -- the fair-price signal, the quote
// lifecycle, the queue-hold refresh policy and the counters -- and the two `final` classes beside it
// each add one behaviour to it: `inventory_aware_l2_market_maker.h` scales and shifts a quote by the
// position, and `active_l2_market_maker.h` halts quoting while the mid is moving fast. Both derive
// from this class rather than wrapping it, because a wrapper has to forward every virtual by hand and
// a forgotten forward changes behaviour silently instead of failing to compile.
//
// The file carries `_base` because the family is not readable from a directory listing otherwise:
// three names ending in `_l2_market_maker.h` look like siblings, and the one every other one extends
// is not the one a reader would pick.

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
    // A commission charged for the instrument's own unit, in USD, and the USD value of one price unit in one
    // quantity unit. Both are here because the gate has to see the whole fee: a US share and a listed future
    // are billed per share and per contract with a zero rate, so a gate that only knew the rate would treat
    // those markets as free and quote everything. The tick's USD value is tick_size * multiplier, so the
    // commission in ticks is fee_per_unit / (tick_size * multiplier), which for a hundred dollar share is a
    // third of a tick and for a micro E-mini is half of one.
    double fee_per_unit{0.0};
    double multiplier{1.0};
    double edge_cover_ticks{0.0};
    // The adverse-selection gate: a side is quoted only when the edge that fill would capture also covers what
    // passive fills at that distance from the midpoint have actually cost in this window, as the pipeline
    // measures it from the market's own prints. It is the second term of the inequality a maker lives by —
    // capture >= fee + adverse selection — and unlike the fee it cannot be read off a schedule, so it arrives
    // as an estimate and it is the reason this gate exists rather than a wider `edge_cover_ticks` setting.
    // False leaves it off, and a run that does not ask must behave exactly as it did before the gate existed.
    bool markout_cover{false};
    // Fills of our own that have to resolve before the measured cover is used. Below it the gate covers the fee
    // only, which is what lets it learn: a gate that tightened from the first decision, on no evidence, would
    // refuse everything and never measure anything.
    uint64_t markout_cover_min_samples{20};
    // Sides refused by the adverse-selection gate, which is the attribution of the same decision.
    uint64_t markout_gate_skips{0};
    // Inventory discipline: above this many units the position is reduced instead of carried, by an order priced
    // at the touch on the reducing side, which the engine fills as a taker. Zero, the default, leaves it off, so
    // every run that does not ask keeps the position it used to carry into the final mark.
    int64_t flatten_band_units{0};
    uint64_t flatten_orders{0};
    uint64_t flatten_quantity{0};
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

    // The pipeline resolves what the market's own prints were worth to a passive fill and pushes the running
    // estimate here. Storing it is all this does: the decision reads it when it prices a quote, which is the
    // moment the distance from the midpoint is known.
    void on_markout_estimate(const MarkoutEstimate& estimate) override
    {
        markout_estimate_ = estimate;
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

        // Inventory discipline, before anything else: a position above the band is reduced rather than carried.
        // The order is priced at the touch on the reducing side, which the engine fills as a taker at that price,
        // so the run pays the spread and the taker fee for the risk it is removing instead of keeping both. It is
        // sent even while quotes rest, because the position it is fixing is the one that already exists.
        if (config_.flatten_band_units > 0 && std::abs(position_) > config_.flatten_band_units)
        {
            const int64_t excess = std::abs(position_) - config_.flatten_band_units;
            const bool buy_to_reduce = position_ < 0;
            orders.emplace_back(buy_to_reduce ? Side::Buy : Side::Sell, symbol,
                                buy_to_reduce ? top.ask_price : top.bid_price,
                                static_cast<uint64_t>(excess), 0);
            ++config_.flatten_orders;
            config_.flatten_quantity += static_cast<uint64_t>(excess);
            last_fair_price_ = (top.bid_price + top.ask_price) / 2.0;
            last_market_mid_ = last_fair_price_;
            quote_age_ = 0;
            queue_activity_ = 0;
            queue_hold_count_ = 0;
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
        // reference price, and the cost it has to cover is the maker fee expressed in ticks at that price,
        // plus, when the run asked for it, what passive fills at that distance have actually cost.
        const double reference_price = 0.5 * (top.bid_price + top.ask_price);
        // The fee a fill has to cover, in ticks: the rate on the reference notional, plus a commission
        // charged for the unit itself, which is how a share and a listed future are billed and which a rate
        // cannot express. The tick is worth tick_size * multiplier in USD, so dividing the commission by that
        // puts it in the same units as the edge. An instrument with no commission leaves the second term
        // exactly zero, so no recorded run moves.
        const double tick_usd =
            config_.tick_size * (config_.multiplier > 0.0 ? config_.multiplier : 1.0);
        const double fee_ticks = reference_price > 0.0 && config_.tick_size > 0.0
                                     ? config_.maker_fee_rate * reference_price / config_.tick_size +
                                           (tick_usd > 0.0 ? config_.fee_per_unit / tick_usd : 0.0)
                                     : 0.0;
        // A measured markout is a signed number, and the gate only cares about the cost: a distance whose
        // passive fills have been profitable so far needs no cover beyond the fee, and one whose fills have
        // lost money needs the loss covered. The bucket is the same one the execution quality line reports, so
        // the two numbers can be read against each other.
        const auto adverse_ticks_at = [&](double distance_ticks, bool buy) {
            const bool far = distance_ticks >= 1.0;
            const double measured = buy ? (far ? markout_estimate_.buy_far_ticks
                                               : markout_estimate_.buy_near_ticks)
                                        : (far ? markout_estimate_.sell_far_ticks
                                               : markout_estimate_.sell_near_ticks);
            return std::max(0.0, -measured);
        };
        const double buy_adverse_ticks =
            config_.markout_cover && markout_estimate_.samples >= config_.markout_cover_min_samples
                ? adverse_ticks_at((reference_price - bid_price) / config_.tick_size, true)
                : 0.0;
        const double sell_adverse_ticks =
            config_.markout_cover && markout_estimate_.samples >= config_.markout_cover_min_samples
                ? adverse_ticks_at((ask_price - reference_price) / config_.tick_size, false)
                : 0.0;
        const double required_buy_edge_ticks =
            fee_ticks + config_.edge_cover_ticks + buy_adverse_ticks;
        const double required_sell_edge_ticks =
            fee_ticks + config_.edge_cover_ticks + sell_adverse_ticks;
        // Either setting turns the gate on; both at zero leave it off, which is what every recorded run asks
        // for, so the documented numbers do not move.
        if (config_.edge_cover_ticks > 0.0 || config_.markout_cover)
        {
            if ((reference_price - bid_price) / config_.tick_size < required_buy_edge_ticks)
            {
                if (buy_quantity > 0) ++config_.edge_gate_skips;
                if (buy_quantity > 0 && buy_adverse_ticks > 0.0) ++config_.markout_gate_skips;
                buy_quantity = 0;
            }
            if ((ask_price - reference_price) / config_.tick_size < required_sell_edge_ticks)
            {
                if (sell_quantity > 0) ++config_.edge_gate_skips;
                if (sell_quantity > 0 && sell_adverse_ticks > 0.0) ++config_.markout_gate_skips;
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
    MarkoutEstimate markout_estimate_;

   public:
    // How often the decision layer refused to quote, so a run can report what it declined to do rather than only
    // what it did. Exposed through accessors because the counters themselves live in the configuration.
    [[nodiscard]] uint64_t edge_gate_skips() const { return config_.edge_gate_skips; }
    [[nodiscard]] uint64_t min_spread_skips() const { return config_.min_spread_skips; }
    [[nodiscard]] uint64_t markout_gate_skips() const override { return config_.markout_gate_skips; }
    [[nodiscard]] uint64_t flatten_orders() const override { return config_.flatten_orders; }
    [[nodiscard]] uint64_t flatten_quantity() const override { return config_.flatten_quantity; }
};
