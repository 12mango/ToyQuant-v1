#include "orderbook/l2_orderbook.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

void L2OrderBook::apply_snapshot(const MarketDepthSnapshot& snapshot)
{
    if (snapshot.symbol.empty()) throw std::invalid_argument("L2 symbol cannot be empty");
    if (snapshot.ts == 0 || snapshot.sequence == 0)
        throw std::invalid_argument("L2 snapshot timestamp and sequence must be positive");

    std::lock_guard<std::mutex> lock(mutex_);
    if (!symbol_.empty() && snapshot.symbol != symbol_)
        throw std::invalid_argument("L2 snapshot symbol changed");
    if (sequence_ != 0 && snapshot.sequence <= sequence_)
        throw std::invalid_argument("L2 snapshot sequence is not increasing");
    if (timestamp_ != 0 && snapshot.ts < timestamp_)
        throw std::invalid_argument("L2 snapshot timestamp moved backwards");

    BidLevels next_bids;
    AskLevels next_asks;
    double previous_bid = 0.0;
    for (const auto& level : snapshot.bids)
    {
        if (!std::isfinite(level.price) || level.price <= 0.0 || level.quantity == 0)
            throw std::invalid_argument("invalid L2 bid level");
        if (!next_bids.empty() && level.price >= previous_bid)
            throw std::invalid_argument("L2 bids must be strictly descending");
        next_bids.emplace(level.price, level.quantity);
        previous_bid = level.price;
    }

    double previous_ask = 0.0;
    for (const auto& level : snapshot.asks)
    {
        if (!std::isfinite(level.price) || level.price <= 0.0 || level.quantity == 0)
            throw std::invalid_argument("invalid L2 ask level");
        if (!next_asks.empty() && level.price <= previous_ask)
            throw std::invalid_argument("L2 asks must be strictly ascending");
        next_asks.emplace(level.price, level.quantity);
        previous_ask = level.price;
    }
    if (next_bids.empty() || next_asks.empty())
        throw std::invalid_argument("L2 snapshot must contain both sides");
    if (next_bids.begin()->first > next_asks.begin()->first)
        throw std::invalid_argument("L2 snapshot is crossed");

    symbol_ = snapshot.symbol;
    bids_ = std::move(next_bids);
    asks_ = std::move(next_asks);
    timestamp_ = snapshot.ts;
    sequence_ = snapshot.sequence;
}

TopOfBook L2OrderBook::top_of_book() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (bids_.empty() || asks_.empty()) return {};
    return TopOfBook{bids_.begin()->first, bids_.begin()->second, asks_.begin()->first,
                     asks_.begin()->second};
}

L2MarketView L2OrderBook::market_view(std::size_t levels) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    L2MarketView view{};
    view.symbol = symbol_;
    view.ts = timestamp_;
    if (bids_.empty() || asks_.empty() || levels == 0) return view;

    view.top = TopOfBook{bids_.begin()->first, bids_.begin()->second, asks_.begin()->first,
                         asks_.begin()->second};
    const auto bid_count = std::min(levels, bids_.size());
    const auto ask_count = std::min(levels, asks_.size());
    auto bid_iterator = bids_.begin();
    for (std::size_t index = 0; index < bid_count; ++index, ++bid_iterator)
        view.bid_depth += bid_iterator->second;
    auto ask_iterator = asks_.begin();
    for (std::size_t index = 0; index < ask_count; ++index, ++ask_iterator)
        view.ask_depth += ask_iterator->second;

    const uint64_t total_depth = view.bid_depth + view.ask_depth;
    if (total_depth > 0)
        view.depth_imbalance = static_cast<double>(view.bid_depth) /
                                   static_cast<double>(total_depth) * 2.0 -
                               1.0;
    const uint64_t top_depth = view.top.bid_size + view.top.ask_size;
    if (top_depth > 0)
        view.micro_price = (view.top.ask_price * static_cast<double>(view.top.bid_size) +
                            view.top.bid_price * static_cast<double>(view.top.ask_size)) /
                           static_cast<double>(top_depth);
    return view;
}

DepthLevel L2OrderBook::level(Side side, std::size_t index) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (side == Side::Buy)
    {
        if (index >= bids_.size()) return {};
        auto iterator = bids_.begin();
        std::advance(iterator, static_cast<std::ptrdiff_t>(index));
        return DepthLevel{iterator->first, iterator->second};
    }
    if (side == Side::Sell)
    {
        if (index >= asks_.size()) return {};
        auto iterator = asks_.begin();
        std::advance(iterator, static_cast<std::ptrdiff_t>(index));
        return DepthLevel{iterator->first, iterator->second};
    }
    return {};
}

std::size_t L2OrderBook::depth(Side side) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (side == Side::Buy) return bids_.size();
    if (side == Side::Sell) return asks_.size();
    return 0;
}

uint64_t L2OrderBook::sequence() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
}