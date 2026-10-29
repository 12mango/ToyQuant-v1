#include "matching_engine.h"

#include <algorithm>
#include <iostream>

#include "common/instrument_spec.h"
#include "execution_report.h"
#include "order.h"

using exchange::Order;

void MatchingEngine::report_trade(const exchange::Order& order, double price, uint64_t quantity,
                                  LiquidityRole liquidity_role, uint64_t ts)
{
    const double rate = liquidity_role == LiquidityRole::Maker ? fee_schedule_.maker_rate
                                                               : fee_schedule_.taker_rate;
    // Fees are charged on the USD value of the filled quantity. For a spot style instrument that
    // value is price * quantity. For a contract with a fixed USD face value it is the face value
    // times the number of contracts, so a Deribit perp contract pays a fee on 10 USD rather than
    // on the BTC price. fee_on_fill is the one place that says so, because this engine is not the
    // only caller that has to book a fill and charge for it.
    report(ExecutionReport{.order_id = order.id,
                           .side = order.side,
                           .exec_type = ExecType::Trade,
                           .symbol = order.symbol,
                           .price = price,
                           .quantity = quantity,
                           .ts = ts,
                           .owner = order.owner,
                           .liquidity_role = liquidity_role,
                           .fee = fee_on_fill(rate, fee_schedule_.unit_notional_usd, fee_schedule_.multiplier,
                                              fee_schedule_.fee_per_share, price, quantity,
                                              fee_schedule_.quantity_scale) +
                                  schedule_fee_on_fill(fee_schedule_, price, quantity,
                                                       order.side == exchange::Side::Sell,
                                                       liquidity_role == LiquidityRole::Maker)});
}

uint64_t MatchingEngine::queue_ahead_at(const std::string& symbol, const Side side,
                                        const double price) const
{
    const SymbolId id = find_symbol(symbol);
    if (id == kUnknownSymbol) return 0;
    const auto book_it = books_.find(id);
    if (book_it == books_.end()) return 0;
    const PriceTick tick = to_price_tick(price, tick_size_);
    if (side == Side::Buy)
    {
        const auto level = book_it->second.bids.find(tick);
        return level == book_it->second.bids.end() ? 0 : level->second.external_queue_ahead;
    }
    const auto level = book_it->second.asks.find(tick);
    return level == book_it->second.asks.end() ? 0 : level->second.external_queue_ahead;
}

void MatchingEngine::send_order(const exchange::Order& order)
{
    if (order_latency_us_ == 0)
    {
        send_order_now(order);
        return;
    }
    // Held until the clock reaches it. The delivery time is taken from the order's own timestamp, so a
    // replay of a feed at a different rate injects the same latency rather than a different one.
    in_flight_orders_.emplace_back(order.ts + order_latency_us_, order);
}

void MatchingEngine::advance_to(uint64_t ts)
{
    // The clock has to move even when nothing is in flight, or a cancel latency would never expire.
    now_us_ = std::max(now_us_, ts);
    if (in_flight_orders_.empty()) return;
    std::stable_sort(in_flight_orders_.begin(), in_flight_orders_.end(),
                     [](const auto& left, const auto& right)
                     { return left.first < right.first; });
    while (!in_flight_orders_.empty() && in_flight_orders_.front().first <= ts)
    {
        const exchange::Order order = in_flight_orders_.front().second;
        in_flight_orders_.erase(in_flight_orders_.begin());
        // The landing is the synchronous path, so latency decides when an order exists and nothing
        // about how it behaves once it does.
        send_order_now(order);
    }
}

void MatchingEngine::reject_order(const exchange::Order& order, const char* reason)
{
    ++rejected_orders_;
    std::cerr << "[ENGINE_REJECT] order=" << order.id << " symbol=" << order.symbol
              << " qty=" << order.qty << " reason=" << reason << " lot_size=" << lot_size_
              << " min_order_quantity=" << min_order_quantity_ << "\n";
}

void MatchingEngine::send_order_now(const exchange::Order& order)
{
    // Every refusal is counted and printed. A rejection used to be a silent return, so a strategy that
    // believed it had a quote resting had none, the pipeline's reserved quantity was never released, and
    // the instrument's own ordering constraints were not enforced at all because the engine had never been
    // told them.
    if (order.id == 0 || order.qty == 0 || order.remaining == 0 || order.remaining > order.qty ||
        order_index_.contains(order.id))
    {
        reject_order(order, order.id == 0 ? "missing id"
                            : order_index_.contains(order.id) ? "duplicate id"
                                                              : "malformed quantities");
        return;
    }
    if (order.qty % lot_size_ != 0 || order.qty < min_order_quantity_)
    {
        reject_order(order, "below the instrument's lot size or minimum quantity");
        return;
    }

    const SymbolId id = symbol_id(order.symbol);
    auto& book = books_[id];
    const bool immediate = order.time_in_force != TimeInForce::Gtc;

    // FOK has to know the whole quantity is available before it trades anything, because a partial fill
    // followed by a cancel is exactly what fill-or-kill forbids. The check uses the same external capacity
    // and the same book the matcher is about to use; a self-trade stops the count because the matcher will
    // refuse to cross that order.
    if (order.time_in_force == TimeInForce::Fok)
    {
        const uint64_t available = external_available(order, id) + internal_available(book, order);
        if (available < order.remaining)
        {
            report(ExecutionReport{.order_id = order.id,
                                   .side = order.side,
                                   .exec_type = ExecType::Cancelled,
                                   .symbol = order.symbol,
                                   .price = order.price,
                                   .quantity = order.remaining,
                                   .ts = order.ts,
                                   .owner = order.owner,
                                   .cancel_reason = CancelReason::Fok});
            return;
        }
    }

    Order remaining_order = order;
    match_external_bbo(id, remaining_order);
    if (remaining_order.remaining == 0) return;

    const uint64_t unfilled = match(book, remaining_order, !immediate);
    if (unfilled > 0 && immediate)
    {
        // The remainder of an immediate order never rests, so there is nothing to remove from the book:
        // the terminal report is the only record that it ended. An IOC cancel is expected flow, while a
        // FOK cancel is the all-or-nothing condition failing.
        report(ExecutionReport{.order_id = order.id,
                               .side = order.side,
                               .exec_type = ExecType::Cancelled,
                               .symbol = order.symbol,
                               .price = order.price,
                               .quantity = unfilled,
                               .ts = order.ts,
                               .owner = order.owner,
                               .cancel_reason = order.time_in_force == TimeInForce::Ioc
                                                    ? CancelReason::Ioc
                                                    : CancelReason::Fok});
    }
}

bool MatchingEngine::external_touch(const exchange::Order& order, SymbolId id,
                                    double& execution_price, uint64_t& displayed_quantity) const
{
    const auto quote_it = external_bbo_.find(id);
    if (quote_it == external_bbo_.end() || order.type != exchange::OrderType::Limit) return false;
    const BboQuote& quote = quote_it->second;
    if (order.side == exchange::Side::Buy && quote.ask_quantity > 0 &&
        to_price_tick(order.price, tick_size_) >= to_price_tick(quote.ask_price, tick_size_))
    {
        execution_price = quote.ask_price;
        displayed_quantity = quote.ask_quantity;
        return true;
    }
    if (order.side == exchange::Side::Sell && quote.bid_quantity > 0 &&
        to_price_tick(order.price, tick_size_) <= to_price_tick(quote.bid_price, tick_size_))
    {
        execution_price = quote.bid_price;
        displayed_quantity = quote.bid_quantity;
        return true;
    }
    return false;
}

uint64_t MatchingEngine::capped_external_quantity(uint64_t displayed_quantity) const
{
    if (participation_limit_ >= 1.0) return displayed_quantity;
    return static_cast<uint64_t>(static_cast<double>(displayed_quantity) * participation_limit_);
}

uint64_t MatchingEngine::external_available(const exchange::Order& order, SymbolId id) const
{
    double execution_price = 0.0;
    uint64_t displayed = 0;
    if (!external_touch(order, id, execution_price, displayed)) return 0;
    return capped_external_quantity(displayed);
}

uint64_t MatchingEngine::internal_available(const MEOrderBook& book, const exchange::Order& order) const
{
    uint64_t available = 0;
    const PriceTick limit = to_price_tick(order.price, tick_size_);
    const auto add_level = [&](const PriceLevel& level)
    {
        for (const auto& resting : level.orders)
        {
            if (is_self_trade(resting, order)) return false;
            available += resting.remaining;
            if (available >= order.remaining) return false;
        }
        return true;
    };

    if (order.side == exchange::Side::Buy)
    {
        for (auto it = book.asks.begin(); it != book.asks.end() && it->first <= limit; ++it)
        {
            if (!add_level(it->second)) break;
        }
    }
    else
    {
        for (auto it = book.bids.begin(); it != book.bids.end() && it->first >= limit; ++it)
        {
            if (!add_level(it->second)) break;
        }
    }
    return std::min(available, order.remaining);
}

// The symbol reaches the engine as a string once per entry point and is resolved to an id there, so
// the body below touches integers only. process_l2_top resolves the same symbol for its own state
// and hands the id over, which is why the body is separate from the public override.
void MatchingEngine::process_bbo_for(SymbolId id, const BboQuote& quote)
{
    const auto previous_quote = external_bbo_.find(id);
    const bool has_previous = previous_quote != external_bbo_.end();
    // One conversion per quoted price, instead of the four that the two comparisons and the queue
    // update below used to do separately.
    const PriceTick bid_tick = to_price_tick(quote.bid_price, tick_size_);
    const PriceTick ask_tick = to_price_tick(quote.ask_price, tick_size_);
    const bool same_bid = has_previous &&
                          to_price_tick(previous_quote->second.bid_price, tick_size_) == bid_tick;
    const bool same_ask = has_previous &&
                          to_price_tick(previous_quote->second.ask_price, tick_size_) == ask_tick;
    const uint64_t bid_reduction = same_bid &&
                                           previous_quote->second.bid_quantity > quote.bid_quantity
                                       ? previous_quote->second.bid_quantity - quote.bid_quantity
                                       : 0;
    const uint64_t ask_reduction = same_ask &&
                                           previous_quote->second.ask_quantity > quote.ask_quantity
                                       ? previous_quote->second.ask_quantity - quote.ask_quantity
                                       : 0;
    // Read the previous sizes before the store below. Assigning into external_bbo_ can rehash the
    // container, and the queue update further down used to dereference the same iterator again
    // afterwards. That was safe only because a rehash cannot happen once the key exists.
    const uint64_t previous_bid_quantity = has_previous ? previous_quote->second.bid_quantity : 0;
    const uint64_t previous_ask_quantity = has_previous ? previous_quote->second.ask_quantity : 0;
    const bool bid_has_public_trade =
        last_bid_trade_ts_[id] > (has_previous ? previous_quote->second.ts : 0);
    const bool ask_has_public_trade =
        last_ask_trade_ts_[id] > (has_previous ? previous_quote->second.ts : 0);
    external_bbo_[id] = quote;

    if (!queue_ahead_ids_.contains(id)) return;
    const auto book_it = books_.find(id);
    if (book_it == books_.end()) return;
    // The conservative model removes nothing from the queue, so the section below is skipped rather than
    // run twice per update to multiply by zero. `QueueAheadModel` answers the same for that model, so the
    // two agree about it; this is the fast path, not a second definition.
    if (queue_ahead_model_.removes_nothing()) return;

    // The arithmetic behind the four models is in `queue_model.h`. What happens here is the application of
    // it to the level that moved, once per side, and the two counters that say how much of the queue was
    // taken out by a displayed decrease rather than by a public trade.
    const auto apply_quantity_change = [&](Side side, PriceTick price, uint64_t reduction,
                                           uint64_t previous_quantity)
    {
        if (reduction == 0) return;
        uint64_t* queue_ahead = nullptr;
        if (side == Side::Buy)
        {
            const auto level = book_it->second.bids.find(price);
            if (level != book_it->second.bids.end())
                queue_ahead = &level->second.external_queue_ahead;
        }
        else
        {
            const auto level = book_it->second.asks.find(price);
            if (level != book_it->second.asks.end())
                queue_ahead = &level->second.external_queue_ahead;
        }
        if (queue_ahead == nullptr || *queue_ahead == 0) return;
        const uint64_t inferred = std::min(
            *queue_ahead,
            queue_ahead_model_.advance_quantity(reduction, previous_quantity, *queue_ahead));
        *queue_ahead -= inferred;
        if (side == Side::Buy)
            buy_queue_from_quantity_changes_ += inferred;
        else
            sell_queue_from_quantity_changes_ += inferred;
    };
    if (!bid_has_public_trade)
        apply_quantity_change(Side::Buy, bid_tick, bid_reduction, previous_bid_quantity);
    if (!ask_has_public_trade)
        apply_quantity_change(Side::Sell, ask_tick, ask_reduction, previous_ask_quantity);
}

void MatchingEngine::process_bbo(const BboQuote& quote)
{
    ++event_counter_;
    // The clock advances before the event is applied, so an order whose latency expired at this
    // timestamp is in the book by the time this event can trade against it.
    advance_to(quote.ts);
    apply_pending_cancels();
    if (!is_usable_quote(quote))
    {
        // An unusable quote drops the stored one. A symbol the engine has never seen has nothing to
        // drop, so this lookup deliberately does not register it.
        const SymbolId id = find_symbol(quote.symbol);
        if (id != kUnknownSymbol) external_bbo_.erase(id);
        return;
    }
    process_bbo_for(symbol_id(quote.symbol), quote);
}

void MatchingEngine::process_l2_top(const BboQuote& quote)
{
    ++event_counter_;
    advance_to(quote.ts);
    apply_pending_cancels();
    const SymbolId id = symbol_id(quote.symbol);
    queue_ahead_ids_.insert(id);

    // The two quotes below are also the keys this side writes into the book, so the conversions are
    // shared instead of repeated once per lookup.
    const PriceTick bid_tick = to_price_tick(quote.bid_price, tick_size_);
    const PriceTick ask_tick = to_price_tick(quote.ask_price, tick_size_);
    const auto previous = l2_top_bbo_.find(id);
    const bool bid_moved = previous == l2_top_bbo_.end() ||
                           to_price_tick(previous->second.bid_price, tick_size_) != bid_tick;
    const bool ask_moved = previous == l2_top_bbo_.end() ||
                           to_price_tick(previous->second.ask_price, tick_size_) != ask_tick;
    l2_top_bbo_[id] = quote;
    if (is_usable_quote(quote)) process_bbo_for(id, quote);
    else external_bbo_.erase(id);

    auto book_it = books_.find(id);
    if (book_it == books_.end()) return;
    if (bid_moved)
    {
        const auto level = book_it->second.bids.find(bid_tick);
        if (level != book_it->second.bids.end())
            level->second.external_queue_ahead = scale_arrival(quote.bid_quantity);
    }
    if (ask_moved)
    {
        const auto level = book_it->second.asks.find(ask_tick);
        if (level != book_it->second.asks.end())
            level->second.external_queue_ahead = scale_arrival(quote.ask_quantity);
    }
}

uint64_t MatchingEngine::scale_arrival(uint64_t displayed) const
{
    // 1.0 returns the value untouched, which is what keeps every recorded run byte-identical when the
    // parameter is left alone.
    if (arrival_share_ >= 1.0) return displayed;
    return static_cast<uint64_t>(static_cast<double>(displayed) * arrival_share_ + 0.5);
}

uint64_t MatchingEngine::displayed_quantity_ahead(const Order& order) const
{
    const SymbolId id = find_symbol(order.symbol);
    if (id == kUnknownSymbol || !queue_ahead_ids_.contains(id)) return 0;
    const auto quote_it = external_bbo_.find(id);
    if (quote_it == external_bbo_.end()) return 0;

    const auto& quote = quote_it->second;
    // A resting order is never first in the queue: the side it joins already shows size. The
    // displayed best size of that side is the only estimate a top-of-book feed provides, and it
    // is the minimum honest one.
    //
    // This used to return 0 whenever the order price was not exactly the best price. An order
    // resting behind the touch then had no queue in front of it and filled in full as soon as
    // any trade printed through its level, which is both wrong and selectively optimistic: those
    // are exactly the prints where the market is moving against the resting side.
    if (order.side == exchange::Side::Buy) return scale_arrival(quote.bid_quantity);
    return scale_arrival(quote.ask_quantity);
}

void MatchingEngine::match_external_bbo(SymbolId id, Order& order)
{
    double execution_price = 0.0;
    uint64_t displayed = 0;
    if (!external_touch(order, id, execution_price, displayed)) return;

    const uint64_t traded = std::min(order.remaining, capped_external_quantity(displayed));
    if (traded == 0) return;
    order.remaining -= traded;

    BboQuote& quote = external_bbo_.find(id)->second;
    (order.side == exchange::Side::Buy ? quote.ask_quantity : quote.bid_quantity) -= traded;

    report_trade(order, execution_price, traded, LiquidityRole::Taker, order.ts);
    report(ExecutionReport{
        .order_id = order.id,
        .side = order.side,
        .exec_type = order.remaining == 0 ? ExecType::Filled : ExecType::PartialFill,
        .symbol = order.symbol,
        .price = order.price,
        .quantity = order.remaining,
        .ts = order.ts,
        .owner = order.owner});
}

void MatchingEngine::process_market_trade(const MarketTrade& trade)
{
    ++event_counter_;
    advance_to(trade.ts);
    const SymbolId id = symbol_id(trade.symbol);
    if (trade.aggressor_side == Side::Sell)
        last_bid_trade_ts_[id] = std::max(last_bid_trade_ts_[id], trade.ts);
    else if (trade.aggressor_side == Side::Buy)
        last_ask_trade_ts_[id] = std::max(last_ask_trade_ts_[id], trade.ts);
    const auto quote_it = external_bbo_.find(id);
    // Consuming the displayed size here is separate from the queue model. Whenever a public trade
    // prints at the best bid or ask, that much of the displayed size is gone, and a later taker
    // order must not be allowed to fill against liquidity that has already traded.
    //
    // This block used to be gated on queue_ahead_ids_ as well, which only process_l2_top
    // populates. On the L1 path that gate was never satisfied, so the stored best bid and ask
    // quantities never shrank from public trades and taker fills were capped by a stale size.
    if (quote_it != external_bbo_.end())
    {
        auto& quote = quote_it->second;
        if (trade.aggressor_side == Side::Sell &&
            to_price_tick(trade.price, tick_size_) == to_price_tick(quote.bid_price, tick_size_))
            quote.bid_quantity = quote.bid_quantity > trade.quantity
                                     ? quote.bid_quantity - trade.quantity
                                     : 0;
        else if (trade.aggressor_side == Side::Buy &&
                 to_price_tick(trade.price, tick_size_) ==
                     to_price_tick(quote.ask_price, tick_size_))
            quote.ask_quantity = quote.ask_quantity > trade.quantity
                                     ? quote.ask_quantity - trade.quantity
                                     : 0;
    }
    process_market_order(id, trade.symbol, trade.aggressor_side, trade.price, trade.quantity,
                         trade.ts);
    apply_pending_cancels();
}

void MatchingEngine::process_market_order(SymbolId id, const std::string& symbol, Side side,
                                          double price, uint64_t quantity, uint64_t ts)
{
    if (side == Side::Unknown || quantity == 0) return;

    // External market ticks are treated as synthetic market orders from the venue.
    exchange::Order market_order{
        0,
        symbol,
        side == Side::Buy ? exchange::Side::Buy : exchange::Side::Sell,
        exchange::OrderType::Market,
        price,
        quantity,
        quantity,
        ts,
        "Market"};
    auto& book = books_[id];
    (void)match(book, market_order, false);
}

void MatchingEngine::cancel_order(uint64_t order_id)
{
    if (cancel_delay_events_ > 0 || cancel_latency_us_ > 0)
    {
        if (order_index_.contains(order_id))
            pending_cancels_[order_id] = {event_counter_ + cancel_delay_events_,
                                          now_us_ + cancel_latency_us_};
        return;
    }
    cancel_order_immediate(order_id);
}

void MatchingEngine::apply_pending_cancels()
{
    for (auto it = pending_cancels_.begin(); it != pending_cancels_.end();)
    {
        if (it->second.due_event > event_counter_ || it->second.due_time > now_us_)
        {
            ++it;
            continue;
        }
        const uint64_t order_id = it->first;
        it = pending_cancels_.erase(it);
        cancel_order_immediate(order_id);
    }
}

void MatchingEngine::cancel_order_immediate(uint64_t order_id)
{
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return;

    Order* ord = it->second;
    const Order cancelled_order = *ord;
    auto& book = books_[symbol_id(cancelled_order.symbol)];

    const auto remove_from = [&](auto& levels)
    {
        auto pit = levels.find(to_price_tick(cancelled_order.price, tick_size_));
        if (pit == levels.end()) return;
        pit->second.orders.remove_if([&](const Order& o) { return o.id == order_id; });
        if (pit->second.orders.empty()) levels.erase(pit);
    };
    if (cancelled_order.side == exchange::Side::Buy)
        remove_from(book.bids);
    else
        remove_from(book.asks);
    order_index_.erase(it);

    report(ExecutionReport{.order_id = order_id,
                           .side = cancelled_order.side,
                           .exec_type = ExecType::Cancelled,
                           .symbol = cancelled_order.symbol,
                           .price = cancelled_order.price,
                           .quantity = cancelled_order.remaining,
                           .ts = cancelled_order.ts,
                           .owner = cancelled_order.owner,
                           .cancel_reason = CancelReason::User});
}

uint64_t MatchingEngine::match(MEOrderBook& book, const Order& incoming, bool rest_incoming)
{
    Order new_order = incoming;  // Copy because matching updates remaining quantity.
    match_side(book, new_order, rest_incoming, incoming.side == exchange::Side::Buy);
    return new_order.remaining;
}

void MatchingEngine::match_side(MEOrderBook& book, Order& new_order, bool rest_incoming,
                                bool buy_side)
{
    uint64_t& qty = new_order.remaining;
    const uint64_t starting_qty = qty;
    const PriceTick incoming_price = to_price_tick(new_order.price, tick_size_);

    // The two books have different comparators, so a generic lambda is what lets one body serve both
    // without turning the whole engine into a template.
    const auto match_levels = [&](auto& levels)
    {
        while (qty > 0 && !levels.empty())
        {
            // Price-time priority:
            // 1) choose the best executable price first
            // 2) within the same price level, match the earliest resting order first
            auto best_it = levels.begin();
            const PriceTick best_price = best_it->first;
            if (buy_side ? incoming_price < best_price : incoming_price > best_price) break;

            if (new_order.owner == "Market" && best_it->second.external_queue_ahead > 0)
            {
                const uint64_t consumed = std::min(qty, best_it->second.external_queue_ahead);
                qty -= consumed;
                best_it->second.external_queue_ahead -= consumed;
                queue_ahead_consumed_ += consumed;
                uint64_t& side_consumed =
                    buy_side ? sell_queue_ahead_consumed_ : buy_queue_ahead_consumed_;
                side_consumed += consumed;
                uint64_t& side_cleared =
                    buy_side ? sell_queue_ahead_cleared_ : buy_queue_ahead_cleared_;
                if (best_it->second.external_queue_ahead == 0 &&
                    !best_it->second.orders.empty())
                    ++side_cleared;
                if (qty == 0) break;
            }

            auto& queue = best_it->second.orders;
            while (qty > 0 && !queue.empty())
            {
                Order& resting = queue.front();
                if (is_self_trade(resting, new_order))
                {
                    report(ExecutionReport{.order_id = new_order.id,
                                           .side = new_order.side,
                                           .exec_type = ExecType::Cancelled,
                                           .symbol = new_order.symbol,
                                           .price = new_order.price,
                                           .quantity = qty,
                                           .ts = new_order.ts,
                                           .owner = new_order.owner,
                                           .cancel_reason = CancelReason::SelfTrade});
                    qty = 0;
                    break;
                }
                const uint64_t traded = std::min(qty, resting.remaining);
                report_trade(new_order, to_price(best_price, tick_size_), traded,
                             LiquidityRole::Taker, new_order.ts);
                report_trade(resting, to_price(best_price, tick_size_), traded,
                             LiquidityRole::Maker, new_order.ts);

                qty -= traded;
                resting.remaining -= traded;
                report(ExecutionReport{
                    .order_id = resting.id,
                    .side = resting.side,
                    .exec_type = resting.remaining == 0 ? ExecType::Filled : ExecType::PartialFill,
                    .symbol = resting.symbol,
                    .price = to_price(best_price, tick_size_),
                    .quantity = resting.remaining,
                    .ts = new_order.ts,
                    .owner = resting.owner});

                if (resting.remaining == 0)
                {
                    order_index_.erase(resting.id);
                    queue.pop_front();
                }
            }
            if (queue.empty()) levels.erase(best_it);
        }
    };

    const auto rest_unfilled = [&](auto& levels)
    {
        auto& level = levels[incoming_price];
        if (level.orders.empty()) level.external_queue_ahead = displayed_quantity_ahead(new_order);
        level.orders.push_back(new_order);
        order_index_[new_order.id] = &level.orders.back();

        report(ExecutionReport{.order_id = new_order.id,
                               .side = new_order.side,
                               .exec_type = ExecType::Resting,
                               .symbol = new_order.symbol,
                               .price = new_order.price,
                               .quantity = qty,
                               .ts = new_order.ts,
                               .owner = new_order.owner});
    };

    if (buy_side)
        match_levels(book.asks);
    else
        match_levels(book.bids);

    if (starting_qty > qty)
    {
        report(ExecutionReport{.order_id = new_order.id,
                               .side = new_order.side,
                               .exec_type = qty == 0 ? ExecType::Filled : ExecType::PartialFill,
                               .symbol = new_order.symbol,
                               .price = new_order.price,
                               .quantity = qty,
                               .ts = new_order.ts,
                               .owner = new_order.owner});
    }

    // Rest any unfilled quantity on our own side of the book.
    if (qty > 0 && rest_incoming)
    {
        if (buy_side)
            rest_unfilled(book.bids);
        else
            rest_unfilled(book.asks);
    }
}
