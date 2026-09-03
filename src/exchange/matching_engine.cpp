#include "matching_engine.h"

#include <algorithm>
#include <iostream>

#include "execution_report.h"
#include "order.h"

using exchange::Order;

void MatchingEngine::send_order(const exchange::Order& order)
{
    if (order.id == 0 || order.qty == 0 || order.remaining == 0 || order.remaining > order.qty ||
        order_index_.contains(order.id))
    {
        return;
    }

    auto& book = books_[order.symbol];
    match(book, order, true);
}

void MatchingEngine::process_market_tick(const Tick& tick)
{
    if (tick.side == Side::Unknown || tick.size == 0) return;

    // External market ticks are treated as synthetic market orders from the venue.
    exchange::Order market_order{
        0,
        tick.symbol,
        tick.side == Side::Buy ? exchange::Side::Buy : exchange::Side::Sell,
        exchange::OrderType::Market,
        tick.price,
        tick.size,
        tick.size,
        tick.ts,
        "Market"};
    auto& book = books_[tick.symbol];
    match(book, market_order, false);
}

void MatchingEngine::cancel_order(uint64_t order_id)
{
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return;

    Order* ord = it->second;
    const Order cancelled_order = *ord;
    auto& book = books_[cancelled_order.symbol];

    if (cancelled_order.side == exchange::Side::Buy)
    {
        auto pit = book.bids.find(cancelled_order.price);
        if (pit != book.bids.end())
        {
            pit->second.orders.remove_if([&](const Order& o) { return o.id == order_id; });
            if (pit->second.orders.empty()) book.bids.erase(pit);
        }
    }
    else
    {
        auto pit = book.asks.find(cancelled_order.price);
        if (pit != book.asks.end())
        {
            pit->second.orders.remove_if([&](const Order& o) { return o.id == order_id; });
            if (pit->second.orders.empty()) book.asks.erase(pit);
        }
    }
    order_index_.erase(it);
    private_order_book_.cancel_order(order_id);

    // ==== 加入 side 字段 ====
    report(ExecutionReport{.order_id = order_id,
                           .side = cancelled_order.side,
                           .exec_type = ExecType::Cancelled,
                           .symbol = cancelled_order.symbol,
                           .price = cancelled_order.price,
                           .quantity = cancelled_order.remaining,
                           .ts = cancelled_order.ts,
                           .owner = cancelled_order.owner});

    std::cout << "Cancel order id=" << order_id << " done.\n";
}

void MatchingEngine::match(MEOrderBook& book, const Order& incoming, bool rest_incoming)
{
    Order new_order = incoming;  // 拷贝，因为要修改 remaining
    uint64_t& qty = new_order.remaining;
    qty = new_order.qty;

    // ==================== 买单匹配逻辑 ====================
    if (incoming.side == exchange::Side::Buy)
    {
        while (qty > 0 && !book.asks.empty())
        {
            // Price-time priority:
            // 1) choose the best price level first (lowest ask for a buy order)
            // 2) within the same price level, match the earliest resting order first
            auto best_ask_it = book.asks.begin();
            double best_price = best_ask_it->first;

            if (new_order.price < best_price) break;

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

                // ==== 加入 side ====
                report(ExecutionReport{.order_id = new_order.id,
                                       .side = new_order.side,
                                       .exec_type = ExecType::Trade,
                                       .symbol = new_order.symbol,
                                       .price = best_price,
                                       .quantity = traded,
                                       .ts = new_order.ts,
                                       .owner = new_order.owner});

                report(ExecutionReport{.order_id = resting.id,
                                       .side = resting.side,
                                       .exec_type = ExecType::Trade,
                                       .symbol = resting.symbol,
                                       .price = best_price,
                                       .quantity = traded,
                                       .ts = new_order.ts,
                                       .owner = resting.owner});

                qty -= traded;
                resting.remaining -= traded;
                private_order_book_.apply_partial_fill(resting.id, traded);

                report(ExecutionReport{
                    .order_id = resting.id,
                    .side = resting.side,
                    .exec_type = resting.remaining == 0 ? ExecType::Filled : ExecType::PartialFill,
                    .symbol = resting.symbol,
                    .price = best_price,
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

        if (incoming.qty > qty)
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

        // 如果买单剩余没成交，挂入订单簿
        if (qty > 0 && rest_incoming)
        {
            book.bids[new_order.price].orders.push_back(new_order);
            order_index_[new_order.id] = &book.bids[new_order.price].orders.back();
            private_order_book_.add_order(new_order);

            // ==== 加入 side ====
            report(ExecutionReport{.order_id = new_order.id,
                                   .side = new_order.side,
                                   .exec_type = ExecType::Resting,
                                   .symbol = new_order.symbol,
                                   .price = new_order.price,
                                   .quantity = qty,
                                   .ts = new_order.ts,
                                   .owner = new_order.owner});

            std::cout << "Resting Buy order id=" << new_order.id << " qty=" << qty << " @ "
                      << new_order.price << "\n";
        }

        // ==================== 卖单匹配逻辑 ====================
    }
    else
    {
        while (qty > 0 && !book.bids.empty())
        {
            // Price-time priority:
            // 1) choose the best price level first (highest bid for a sell order)
            // 2) within the same price level, match the earliest resting order first
            auto best_bid_it = book.bids.begin();
            double best_price = best_bid_it->first;

            if (new_order.price > best_price) break;

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

                // ==== 加入 side ====
                report(ExecutionReport{.order_id = new_order.id,
                                       .side = new_order.side,
                                       .exec_type = ExecType::Trade,
                                       .symbol = new_order.symbol,
                                       .price = best_price,
                                       .quantity = traded,
                                       .ts = new_order.ts,
                                       .owner = new_order.owner});

                report(ExecutionReport{.order_id = resting.id,
                                       .side = resting.side,
                                       .exec_type = ExecType::Trade,
                                       .symbol = resting.symbol,
                                       .price = best_price,
                                       .quantity = traded,
                                       .ts = new_order.ts,
                                       .owner = resting.owner});

                qty -= traded;
                resting.remaining -= traded;
                private_order_book_.apply_partial_fill(resting.id, traded);

                report(ExecutionReport{
                    .order_id = resting.id,
                    .side = resting.side,
                    .exec_type = resting.remaining == 0 ? ExecType::Filled : ExecType::PartialFill,
                    .symbol = resting.symbol,
                    .price = best_price,
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

        if (incoming.qty > qty)
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

        // 如果卖单剩余没成交，挂入订单簿
        if (qty > 0 && rest_incoming)
        {
            book.asks[new_order.price].orders.push_back(new_order);
            order_index_[new_order.id] = &book.asks[new_order.price].orders.back();
            private_order_book_.add_order(new_order);

            // ==== 加入 side ====
            report(ExecutionReport{.order_id = new_order.id,
                                   .side = new_order.side,
                                   .exec_type = ExecType::Resting,
                                   .symbol = new_order.symbol,
                                   .price = new_order.price,
                                   .quantity = qty,
                                   .ts = new_order.ts,
                                   .owner = new_order.owner});

            std::cout << "Resting Sell order id=" << new_order.id << " qty=" << qty << " @ "
                      << new_order.price << "\n";
        }
    }
}
