#include "orderbook/orderbook.h"

#include <algorithm>

uint64_t OrderBook::level_quantity(const std::list<OrderNode>& orders)
{
    uint64_t total = 0;
    for (const auto& node : orders)
    {
        total += node.order.remaining;
    }
    return total;
}

void OrderBook::add_order(const exchange::Order& order)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if (order.id == 0 || order.qty == 0 || order.remaining == 0 || order.remaining > order.qty ||
        state_index_.contains(order.id))
    {
        return;
    }

    auto& book = books_[order.symbol];
    const PriceTick price = to_price_tick(order.price, tick_size_);
    if (order.side == exchange::Side::Buy)
    {
        auto& queue = book.bids_orders[price];
        queue.push_back(OrderNode{order, OrderState::Active});
        order_index_[order.id] = &queue.back();
        state_index_[order.id] = OrderState::Active;
        book.bids_qty[price] += order.remaining;
    }
    else
    {
        auto& queue = book.asks_orders[price];
        queue.push_back(OrderNode{order, OrderState::Active});
        order_index_[order.id] = &queue.back();
        state_index_[order.id] = OrderState::Active;
        book.asks_qty[price] += order.remaining;
    }
}

bool OrderBook::cancel_order(uint64_t order_id)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;

    OrderNode* node = it->second;
    const std::string symbol = node->order.symbol;
    const exchange::Side side = node->order.side;
    const PriceTick price = to_price_tick(node->order.price, tick_size_);
    auto& book = books_[symbol];

    if (side == exchange::Side::Buy)
    {
        auto price_it = book.bids_orders.find(price);
        if (price_it == book.bids_orders.end())
        {
            order_index_.erase(order_id);
            return false;
        }
        const auto removed = price_it->second.remove_if([order_id](const OrderNode& entry)
                                                        { return entry.order.id == order_id; });
        if (removed == 0) return false;
        book.bids_qty[price] = level_quantity(price_it->second);
        if (price_it->second.empty())
        {
            book.bids_orders.erase(price_it);
            book.bids_qty.erase(price);
        }
    }
    else
    {
        auto price_it = book.asks_orders.find(price);
        if (price_it == book.asks_orders.end())
        {
            order_index_.erase(order_id);
            return false;
        }
        const auto removed = price_it->second.remove_if([order_id](const OrderNode& entry)
                                                        { return entry.order.id == order_id; });
        if (removed == 0) return false;
        book.asks_qty[price] = level_quantity(price_it->second);
        if (price_it->second.empty())
        {
            book.asks_orders.erase(price_it);
            book.asks_qty.erase(price);
        }
    }

    state_index_[order_id] = OrderState::Cancelled;
    order_index_.erase(order_id);
    return true;
}

bool OrderBook::apply_partial_fill(uint64_t order_id, uint64_t filled_qty)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = order_index_.find(order_id);
    if (it == order_index_.end()) return false;

    OrderNode* node = it->second;
    if (filled_qty == 0) return true;
    if (filled_qty > node->order.remaining) return false;

    uint64_t remaining_after = node->order.remaining - filled_qty;
    node->order.remaining = remaining_after;

    auto& book = books_[node->order.symbol];
    if (node->order.side == exchange::Side::Buy)
    {
        const PriceTick price = to_price_tick(node->order.price, tick_size_);
        auto price_it = book.bids_orders.find(price);
        if (price_it != book.bids_orders.end())
        {
            book.bids_qty[price] = level_quantity(price_it->second);
        }
    }
    else
    {
        const PriceTick price = to_price_tick(node->order.price, tick_size_);
        auto price_it = book.asks_orders.find(price);
        if (price_it != book.asks_orders.end())
        {
            book.asks_qty[price] = level_quantity(price_it->second);
        }
    }

    if (remaining_after == 0)
    {
        node->state = OrderState::Filled;
        state_index_[order_id] = OrderState::Filled;
        const exchange::Side side = node->order.side;
        const PriceTick price = to_price_tick(node->order.price, tick_size_);
        if (side == exchange::Side::Buy)
        {
            auto price_it = book.bids_orders.find(price);
            if (price_it != book.bids_orders.end())
            {
                price_it->second.remove_if([order_id](const OrderNode& entry)
                                           { return entry.order.id == order_id; });
                if (price_it->second.empty())
                {
                    book.bids_orders.erase(price_it);
                    book.bids_qty.erase(price);
                }
            }
        }
        else
        {
            auto price_it = book.asks_orders.find(price);
            if (price_it != book.asks_orders.end())
            {
                price_it->second.remove_if([order_id](const OrderNode& entry)
                                           { return entry.order.id == order_id; });
                if (price_it->second.empty())
                {
                    book.asks_orders.erase(price_it);
                    book.asks_qty.erase(price);
                }
            }
        }
        order_index_.erase(order_id);
        return true;
    }

    node->state = OrderState::PartialFilled;
    state_index_[order_id] = OrderState::PartialFilled;
    return true;
}

OrderState OrderBook::state_for_order(uint64_t order_id) const
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = state_index_.find(order_id);
    if (it != state_index_.end()) return it->second;
    return OrderState::Rejected;
}

void OrderBook::on_tick(const Tick& t)
{
    std::lock_guard<std::mutex> lk(mtx_);
    auto& b = books_[t.symbol];
    const PriceTick price = to_price_tick(t.price, tick_size_);
    if (t.side == Side::Buy)
    {
        b.bids_qty[price] = t.size;
    }
    else if (t.side == Side::Sell)
    {
        b.asks_qty[price] = t.size;
    }
    else if (t.price > 0)
    {
        b.bids_qty[price] = t.size;
    }
}

void OrderBook::on_bbo(const BboQuote& quote)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto& book = books_[quote.symbol];
    book.external_bbo = quote;
    book.has_external_bbo =
        quote.bid_price > 0.0 && quote.ask_price > 0.0 && quote.bid_price <= quote.ask_price;
}

TopOfBook OrderBook::top(const std::string& symbol)
{
    std::lock_guard<std::mutex> lk(mtx_);
    TopOfBook out;
    auto it = books_.find(symbol);
    if (it == books_.end())
    {
        return out;
    }

    auto& b = it->second;
    if (!b.bids_qty.empty())
    {
        out.bid_price = to_price(b.bids_qty.begin()->first, tick_size_);
        out.bid_size = b.bids_qty.begin()->second;
    }
    if (!b.asks_qty.empty())
    {
        out.ask_price = to_price(b.asks_qty.begin()->first, tick_size_);
        out.ask_size = b.asks_qty.begin()->second;
    }
    if (b.has_external_bbo)
    {
        if (out.bid_price == 0.0 || b.external_bbo.bid_price > out.bid_price)
        {
            out.bid_price = b.external_bbo.bid_price;
            out.bid_size = b.external_bbo.bid_quantity;
        }
        else if (to_price_tick(out.bid_price, tick_size_) ==
                 to_price_tick(b.external_bbo.bid_price, tick_size_))
        {
            out.bid_size += b.external_bbo.bid_quantity;
        }

        if (out.ask_price == 0.0 || b.external_bbo.ask_price < out.ask_price)
        {
            out.ask_price = b.external_bbo.ask_price;
            out.ask_size = b.external_bbo.ask_quantity;
        }
        else if (to_price_tick(out.ask_price, tick_size_) ==
                 to_price_tick(b.external_bbo.ask_price, tick_size_))
        {
            out.ask_size += b.external_bbo.ask_quantity;
        }
    }
    return out;
}
