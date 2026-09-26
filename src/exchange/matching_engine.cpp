#include "matching_engine.h"

#include <algorithm>
#include <iostream>

#include "execution_report.h"
#include "order.h"
#include "utils/logger.h"

using exchange::Order;

void MatchingEngine::report_trade(const exchange::Order& order, double price, uint64_t quantity,
                                  LiquidityRole liquidity_role, uint64_t ts)
{
    const double rate = liquidity_role == LiquidityRole::Maker ? fee_schedule_.maker_rate
                                                               : fee_schedule_.taker_rate;
    const double real_quantity =
        static_cast<double>(quantity) /
        static_cast<double>(std::max<uint64_t>(1, fee_schedule_.quantity_scale));
    report(ExecutionReport{.order_id = order.id,
                           .side = order.side,
                           .exec_type = ExecType::Trade,
                           .symbol = order.symbol,
                           .price = price,
                           .quantity = quantity,
                           .ts = ts,
                           .owner = order.owner,
                           .liquidity_role = liquidity_role,
                           .fee = price * real_quantity * rate});
}

void MatchingEngine::send_order(const exchange::Order& order)
{
    if (order.id == 0 || order.qty == 0 || order.remaining == 0 || order.remaining > order.qty ||
        order_index_.contains(order.id))
    {
        return;
    }

    Order remaining_order = order;
    match_external_bbo(remaining_order);
    if (remaining_order.remaining == 0) return;

    auto& book = books_[remaining_order.symbol];
    match(book, remaining_order, true);
}

void MatchingEngine::process_bbo(const BboQuote& quote)
{
    ++event_counter_;
    apply_pending_cancels();
    if (quote.symbol.empty() || quote.bid_price <= 0.0 || quote.ask_price <= 0.0 ||
        quote.bid_price > quote.ask_price)
    {
        external_bbo_.erase(quote.symbol);
        return;
    }
    const auto previous_quote = external_bbo_.find(quote.symbol);
    const bool has_previous = previous_quote != external_bbo_.end();
    const bool same_bid = has_previous &&
                          to_price_tick(previous_quote->second.bid_price, tick_size_) ==
                              to_price_tick(quote.bid_price, tick_size_);
    const bool same_ask = has_previous &&
                          to_price_tick(previous_quote->second.ask_price, tick_size_) ==
                              to_price_tick(quote.ask_price, tick_size_);
    const uint64_t bid_reduction = same_bid &&
                                           previous_quote->second.bid_quantity > quote.bid_quantity
                                       ? previous_quote->second.bid_quantity - quote.bid_quantity
                                       : 0;
    const uint64_t ask_reduction = same_ask &&
                                           previous_quote->second.ask_quantity > quote.ask_quantity
                                       ? previous_quote->second.ask_quantity - quote.ask_quantity
                                       : 0;
    const bool bid_has_public_trade =
        last_bid_trade_ts_[quote.symbol] > (has_previous ? previous_quote->second.ts : 0);
    const bool ask_has_public_trade =
        last_ask_trade_ts_[quote.symbol] > (has_previous ? previous_quote->second.ts : 0);
    external_bbo_[quote.symbol] = quote;

    if (!queue_ahead_symbols_.contains(quote.symbol)) return;
    const auto book_it = books_.find(quote.symbol);
    if (book_it == books_.end()) return;
    if (queue_model_ == QueueModel::Conservative) return;

    const auto advance_quantity = [this](uint64_t reduction, uint64_t previous_quantity)
    {
        if (queue_model_ == QueueModel::Optimistic) return reduction;
        const uint64_t bounded_reduction = std::min(
            reduction, static_cast<uint64_t>(std::llround(
                           static_cast<double>(previous_quantity) *
                           std::clamp(heuristic_max_reduction_fraction_, 0.0, 1.0))));
        return static_cast<uint64_t>(std::llround(
            static_cast<double>(bounded_reduction) *
            std::clamp(heuristic_cancel_ahead_ratio_, 0.0, 1.0)));
    };
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
        const uint64_t inferred = std::min(*queue_ahead,
                           advance_quantity(reduction, previous_quantity));
        *queue_ahead -= inferred;
        if (side == Side::Buy)
            buy_queue_from_quantity_changes_ += inferred;
        else
            sell_queue_from_quantity_changes_ += inferred;
    };
    if (!bid_has_public_trade)
        apply_quantity_change(Side::Buy, to_price_tick(quote.bid_price, tick_size_), bid_reduction,
                              has_previous ? previous_quote->second.bid_quantity : 0);
    if (!ask_has_public_trade)
        apply_quantity_change(Side::Sell, to_price_tick(quote.ask_price, tick_size_), ask_reduction,
                              has_previous ? previous_quote->second.ask_quantity : 0);
}

void MatchingEngine::process_l2_top(const BboQuote& quote)
{
    queue_ahead_symbols_.insert(quote.symbol);
    const auto previous = l2_top_bbo_.find(quote.symbol);
    const bool bid_moved = previous == l2_top_bbo_.end() ||
                           to_price_tick(previous->second.bid_price, tick_size_) !=
                               to_price_tick(quote.bid_price, tick_size_);
    const bool ask_moved = previous == l2_top_bbo_.end() ||
                           to_price_tick(previous->second.ask_price, tick_size_) !=
                               to_price_tick(quote.ask_price, tick_size_);
    l2_top_bbo_[quote.symbol] = quote;
    process_bbo(quote);
    auto book_it = books_.find(quote.symbol);
    if (book_it == books_.end()) return;
    if (bid_moved)
    {
        const auto level = book_it->second.bids.find(to_price_tick(quote.bid_price, tick_size_));
        if (level != book_it->second.bids.end()) level->second.external_queue_ahead = quote.bid_quantity;
    }
    if (ask_moved)
    {
        const auto level = book_it->second.asks.find(to_price_tick(quote.ask_price, tick_size_));
        if (level != book_it->second.asks.end()) level->second.external_queue_ahead = quote.ask_quantity;
    }
}

uint64_t MatchingEngine::displayed_quantity_ahead(const Order& order) const
{
    if (!queue_ahead_symbols_.contains(order.symbol)) return 0;
    const auto quote_it = external_bbo_.find(order.symbol);
    if (quote_it == external_bbo_.end()) return 0;

    const auto& quote = quote_it->second;
    if (order.side == exchange::Side::Buy &&
        to_price_tick(order.price, tick_size_) == to_price_tick(quote.bid_price, tick_size_))
        return quote.bid_quantity;
    if (order.side == exchange::Side::Sell &&
        to_price_tick(order.price, tick_size_) == to_price_tick(quote.ask_price, tick_size_))
        return quote.ask_quantity;
    return 0;
}

void MatchingEngine::match_external_bbo(Order& order)
{
    const auto quote_it = external_bbo_.find(order.symbol);
    if (quote_it == external_bbo_.end() || order.type != exchange::OrderType::Limit) return;

    BboQuote& quote = quote_it->second;
    double execution_price = 0.0;
    uint64_t* available_quantity = nullptr;
    if (order.side == exchange::Side::Buy && quote.ask_quantity > 0 &&
        to_price_tick(order.price, tick_size_) >= to_price_tick(quote.ask_price, tick_size_))
    {
        execution_price = quote.ask_price;
        available_quantity = &quote.ask_quantity;
    }
    else if (order.side == exchange::Side::Sell && quote.bid_quantity > 0 &&
             to_price_tick(order.price, tick_size_) <= to_price_tick(quote.bid_price, tick_size_))
    {
        execution_price = quote.bid_price;
        available_quantity = &quote.bid_quantity;
    }

    if (!available_quantity) return;

    const uint64_t traded = std::min(order.remaining, *available_quantity);
    order.remaining -= traded;
    *available_quantity -= traded;
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
    if (trade.aggressor_side == Side::Sell)
        last_bid_trade_ts_[trade.symbol] = std::max(last_bid_trade_ts_[trade.symbol], trade.ts);
    else if (trade.aggressor_side == Side::Buy)
        last_ask_trade_ts_[trade.symbol] = std::max(last_ask_trade_ts_[trade.symbol], trade.ts);
    const auto quote_it = external_bbo_.find(trade.symbol);
    if (quote_it != external_bbo_.end() && queue_ahead_symbols_.contains(trade.symbol))
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
    process_market_order(trade.symbol, trade.aggressor_side, trade.price, trade.quantity, trade.ts);
    apply_pending_cancels();
}

void MatchingEngine::process_market_order(const std::string& symbol, Side side, double price,
                                          uint64_t quantity, uint64_t ts)
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
    auto& book = books_[symbol];
    match(book, market_order, false);
}

void MatchingEngine::cancel_order(uint64_t order_id)
{
    if (cancel_delay_events_ > 0)
    {
        if (order_index_.contains(order_id))
            pending_cancels_[order_id] = event_counter_ + cancel_delay_events_;
        return;
    }
    cancel_order_immediate(order_id);
}

void MatchingEngine::apply_pending_cancels()
{
    for (auto it = pending_cancels_.begin(); it != pending_cancels_.end();)
    {
        if (it->second > event_counter_)
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
    auto& book = books_[cancelled_order.symbol];

    if (cancelled_order.side == exchange::Side::Buy)
    {
        auto pit = book.bids.find(to_price_tick(cancelled_order.price, tick_size_));
        if (pit != book.bids.end())
        {
            pit->second.orders.remove_if([&](const Order& o) { return o.id == order_id; });
            if (pit->second.orders.empty()) book.bids.erase(pit);
        }
    }
    else
    {
        auto pit = book.asks.find(to_price_tick(cancelled_order.price, tick_size_));
        if (pit != book.asks.end())
        {
            pit->second.orders.remove_if([&](const Order& o) { return o.id == order_id; });
            if (pit->second.orders.empty()) book.asks.erase(pit);
        }
    }
    order_index_.erase(it);

    report(ExecutionReport{.order_id = order_id,
                           .side = cancelled_order.side,
                           .exec_type = ExecType::Cancelled,
                           .symbol = cancelled_order.symbol,
                           .price = cancelled_order.price,
                           .quantity = cancelled_order.remaining,
                           .ts = cancelled_order.ts,
                           .owner = cancelled_order.owner});

    if (logger_) logger_->debug("[CANCEL] order_id=", order_id);
}

void MatchingEngine::match(MEOrderBook& book, const Order& incoming, bool rest_incoming)
{
    Order new_order = incoming;  // Copy because matching updates remaining quantity.
    uint64_t& qty = new_order.remaining;
    const uint64_t starting_qty = qty;
    const PriceTick incoming_price = to_price_tick(new_order.price, tick_size_);

    // ==================== Buy-Side Matching ====================
    if (incoming.side == exchange::Side::Buy)
    {
        while (qty > 0 && !book.asks.empty())
        {
            // Price-time priority:
            // 1) choose the best price level first (lowest ask for a buy order)
            // 2) within the same price level, match the earliest resting order first
            auto best_ask_it = book.asks.begin();
            PriceTick best_price = best_ask_it->first;

            if (incoming_price < best_price) break;

            if (new_order.owner == "Market" && best_ask_it->second.external_queue_ahead > 0)
            {
                const uint64_t consumed =
                    std::min(qty, best_ask_it->second.external_queue_ahead);
                qty -= consumed;
                best_ask_it->second.external_queue_ahead -= consumed;
                queue_ahead_consumed_ += consumed;
                sell_queue_ahead_consumed_ += consumed;
                if (best_ask_it->second.external_queue_ahead == 0 &&
                    !best_ask_it->second.orders.empty())
                    ++sell_queue_ahead_cleared_;
                if (qty == 0) break;
            }

            auto& ask_queue = best_ask_it->second.orders;
            while (qty > 0 && !ask_queue.empty())
            {
                Order& resting = ask_queue.front();
                if (is_self_trade(resting, new_order))
                {
                    report(ExecutionReport{.order_id = new_order.id,
                                           .side = new_order.side,
                                           .exec_type = ExecType::Cancelled,
                                           .symbol = new_order.symbol,
                                           .price = new_order.price,
                                           .quantity = qty,
                                           .ts = new_order.ts,
                                           .owner = new_order.owner});
                    qty = 0;
                    break;
                }
                uint64_t traded = std::min(qty, resting.remaining);

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
                    ask_queue.pop_front();
                }
            }
            if (ask_queue.empty()) book.asks.erase(best_ask_it);
        }

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

        // Rest any unfilled quantity in the bid book.
        if (qty > 0 && rest_incoming)
        {
            auto& level = book.bids[incoming_price];
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

            if (logger_)
                logger_->debug("[RESTING] side=Buy order_id=", new_order.id, " qty=", qty,
                               " price=", new_order.price);
        }

        // ==================== Sell-Side Matching ====================
    }
    else
    {
        while (qty > 0 && !book.bids.empty())
        {
            // Price-time priority:
            // 1) choose the best price level first (highest bid for a sell order)
            // 2) within the same price level, match the earliest resting order first
            auto best_bid_it = book.bids.begin();
            PriceTick best_price = best_bid_it->first;

            if (incoming_price > best_price) break;

            if (new_order.owner == "Market" && best_bid_it->second.external_queue_ahead > 0)
            {
                const uint64_t consumed =
                    std::min(qty, best_bid_it->second.external_queue_ahead);
                qty -= consumed;
                best_bid_it->second.external_queue_ahead -= consumed;
                queue_ahead_consumed_ += consumed;
                buy_queue_ahead_consumed_ += consumed;
                if (best_bid_it->second.external_queue_ahead == 0 &&
                    !best_bid_it->second.orders.empty())
                    ++buy_queue_ahead_cleared_;
                if (qty == 0) break;
            }

            auto& bid_queue = best_bid_it->second.orders;
            while (qty > 0 && !bid_queue.empty())
            {
                Order& resting = bid_queue.front();
                if (is_self_trade(resting, new_order))
                {
                    report(ExecutionReport{.order_id = new_order.id,
                                           .side = new_order.side,
                                           .exec_type = ExecType::Cancelled,
                                           .symbol = new_order.symbol,
                                           .price = new_order.price,
                                           .quantity = qty,
                                           .ts = new_order.ts,
                                           .owner = new_order.owner});
                    qty = 0;
                    break;
                }
                uint64_t traded = std::min(qty, resting.remaining);

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
                    bid_queue.pop_front();
                }
            }
            if (bid_queue.empty()) book.bids.erase(best_bid_it);
        }

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

        // Rest any unfilled quantity in the ask book.
        if (qty > 0 && rest_incoming)
        {
            auto& level = book.asks[incoming_price];
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

            if (logger_)
                logger_->debug("[RESTING] side=Sell order_id=", new_order.id, " qty=", qty,
                               " price=", new_order.price);
        }
    }
}
