#include "matching_engine.h"

#include <algorithm>
#include <iostream>

#include "execution_report.h"
#include "order.h"

using exchange::Order;

void MatchingEngine::send_order(const exchange::Order& order)
{
    auto& book = books_[order.symbol];
    match(book, order);
}

void MatchingEngine::cancel_order(uint64_t order_id)
{
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return;

    Order* ord = it->second;
    auto& book = books_[ord->symbol];

    if (ord->side == exchange::Side::Buy)
    {
        auto pit = book.bids.find(ord->price);
        if (pit != book.bids.end())
        {
            pit->second.orders.remove_if([&](const Order& o) { return o.id == order_id; });
            if (pit->second.orders.empty()) book.bids.erase(pit);
        }
    }
    else
    {
        auto pit = book.asks.find(ord->price);
        if (pit != book.asks.end())
        {
            pit->second.orders.remove_if([&](const Order& o) { return o.id == order_id; });
            if (pit->second.orders.empty()) book.asks.erase(pit);
        }
    }
    order_index_.erase(it);

    // ==== 加入 side 字段 ====
    report(ExecutionReport{.order_id = order_id,
                           .side = ord->side,
                           .exec_type = ExecType::Cancelled,
                           .symbol = ord->symbol,
                           .price = ord->price,
                           .quantity = ord->remaining,
                           .ts = ord->ts});

    std::cout << "Cancel order id=" << order_id << " done.\n";
}

void MatchingEngine::match(MEOrderBook& book, const Order& incoming)
{
    Order new_order = incoming;  // 拷贝，因为要修改 remaining
    uint64_t& qty = new_order.remaining;
    qty = new_order.qty;

    // ==================== 买单匹配逻辑 ====================
    if (incoming.side == exchange::Side::Buy)
    {
        while (qty > 0 && !book.asks.empty())
        {
            auto best_ask_it = book.asks.begin();  // 卖盘最优价（最低）
            double best_price = best_ask_it->first;

            if (new_order.price < best_price) break;

            auto& ask_queue = best_ask_it->second.orders;
            while (qty > 0 && !ask_queue.empty())
            {
                Order& resting = ask_queue.front();
                uint64_t traded = std::min(qty, resting.remaining);

                // ==== 加入 side ====
                report(ExecutionReport{.order_id = new_order.id,
                                       .side = new_order.side,
                                       .exec_type = ExecType::Trade,
                                       .symbol = new_order.symbol,
                                       .price = best_price,
                                       .quantity = traded,
                                       .ts = new_order.ts});

                qty -= traded;
                resting.remaining -= traded;

                if (resting.remaining == 0)
                {
                    order_index_.erase(resting.id);
                    ask_queue.pop_front();
                }
            }
            if (ask_queue.empty()) book.asks.erase(best_ask_it);
        }

        // 如果买单剩余没成交，挂入订单簿
        if (qty > 0)
        {
            book.bids[new_order.price].orders.push_back(new_order);
            order_index_[new_order.id] = &book.bids[new_order.price].orders.back();

            // ==== 加入 side ====
            report(ExecutionReport{.order_id = new_order.id,
                                   .side = new_order.side,
                                   .exec_type = ExecType::Resting,
                                   .symbol = new_order.symbol,
                                   .price = new_order.price,
                                   .quantity = qty,
                                   .ts = new_order.ts});

            std::cout << "Resting Buy order id=" << new_order.id << " qty=" << qty << " @ "
                      << new_order.price << "\n";
        }

        // ==================== 卖单匹配逻辑 ====================
    }
    else
    {
        while (qty > 0 && !book.bids.empty())
        {
            auto best_bid_it = book.bids.begin();  // 买盘最优价（最高）
            double best_price = best_bid_it->first;

            if (new_order.price > best_price) break;

            auto& bid_queue = best_bid_it->second.orders;
            while (qty > 0 && !bid_queue.empty())
            {
                Order& resting = bid_queue.front();
                uint64_t traded = std::min(qty, resting.remaining);

                // ==== 加入 side ====
                report(ExecutionReport{.order_id = new_order.id,
                                       .side = new_order.side,
                                       .exec_type = ExecType::Trade,
                                       .symbol = new_order.symbol,
                                       .price = best_price,
                                       .quantity = traded,
                                       .ts = new_order.ts});

                qty -= traded;
                resting.remaining -= traded;

                if (resting.remaining == 0)
                {
                    order_index_.erase(resting.id);
                    bid_queue.pop_front();
                }
            }
            if (bid_queue.empty()) book.bids.erase(best_bid_it);
        }

        // 如果卖单剩余没成交，挂入订单簿
        if (qty > 0)
        {
            book.asks[new_order.price].orders.push_back(new_order);
            order_index_[new_order.id] = &book.asks[new_order.price].orders.back();

            // ==== 加入 side ====
            report(ExecutionReport{.order_id = new_order.id,
                                   .side = new_order.side,
                                   .exec_type = ExecType::Resting,
                                   .symbol = new_order.symbol,
                                   .price = new_order.price,
                                   .quantity = qty,
                                   .ts = new_order.ts});

            std::cout << "Resting Sell order id=" << new_order.id << " qty=" << qty << " @ "
                      << new_order.price << "\n";
        }
    }
}
