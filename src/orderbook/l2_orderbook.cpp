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

    // Built aside and swapped in, so a snapshot that fails validation leaves the live book as it
    // was. Each ladder sizes its range around the levels it is given.
    TickLadder next_bids(true);
    TickLadder next_asks(false);
    PriceTick previous = 0;
    bool have_previous = false;
    for (const auto& level : snapshot.bids)
    {
        if (!std::isfinite(level.price) || level.price <= 0.0 || level.quantity == 0)
            throw std::invalid_argument("invalid L2 bid level");
        const PriceTick price = to_price_tick(level.price, tick_size_);
        if (price <= 0 || (have_previous && price >= previous))
            throw std::invalid_argument("L2 bids must be strictly descending");
        next_bids.set(price, level.quantity);
        previous = price;
        have_previous = true;
    }
    previous = 0;
    have_previous = false;
    for (const auto& level : snapshot.asks)
    {
        if (!std::isfinite(level.price) || level.price <= 0.0 || level.quantity == 0)
            throw std::invalid_argument("invalid L2 ask level");
        const PriceTick price = to_price_tick(level.price, tick_size_);
        if (price <= 0 || (have_previous && price <= previous))
            throw std::invalid_argument("L2 asks must be strictly ascending");
        next_asks.set(price, level.quantity);
        previous = price;
        have_previous = true;
    }
    if (next_bids.empty() || next_asks.empty())
        throw std::invalid_argument("L2 snapshot must contain both sides");
    if (next_bids.best() > next_asks.best())
        throw std::invalid_argument("L2 snapshot is crossed");

    symbol_ = snapshot.symbol;
    bids_ = std::move(next_bids);
    asks_ = std::move(next_asks);
    timestamp_ = snapshot.ts;
    local_timestamp_ = 0;
    sequence_ = snapshot.sequence;
}

void L2OrderBook::apply_incremental_batch(const IncrementalBookBatch& batch)
{
    if (batch.updates.empty()) return;
    if (batch.symbol.empty() || batch.exchange_ts == 0 || batch.local_ts == 0)
        throw std::invalid_argument("invalid incremental L2 batch metadata");

    std::lock_guard<std::mutex> lock(mutex_);
    if (symbol_.empty()) symbol_ = batch.symbol;
    if (batch.symbol != symbol_) throw std::invalid_argument("L2 incremental symbol changed");
    if (timestamp_ != 0 && batch.exchange_ts < timestamp_)
        throw std::invalid_argument("L2 incremental timestamp moved backwards");
    if (local_timestamp_ != 0 && batch.local_ts < local_timestamp_)
        throw std::invalid_argument("L2 incremental local timestamp moved backwards");

    const bool reset = batch.has_snapshot();
    if (reset && !snapshot_batch_active_)
    {
        bids_.clear();
        asks_.clear();
    }
    snapshot_batch_active_ = reset;

    for (const auto& update : batch.updates)
    {
        if (update.exchange_ts == 0 || update.local_ts != batch.local_ts ||
            update.side == Side::Unknown ||
            !std::isfinite(update.price) || update.price <= 0.0)
            throw std::invalid_argument("invalid L2 incremental update");

        const PriceTick price = to_price_tick(update.price, tick_size_);
        if (price <= 0) throw std::invalid_argument("invalid L2 incremental price");
        // A zero amount removes the level, which is what set() does with a zero quantity.
        if (update.side == Side::Buy) bids_.set(price, update.amount);
        else asks_.set(price, update.amount);
    }

    timestamp_ = batch.exchange_ts;
    local_timestamp_ = batch.local_ts;
    sequence_++;
}

TopOfBook L2OrderBook::top_of_book() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const TickLadder::TopLevels bid = bids_.top_levels(1);
    const TickLadder::TopLevels ask = asks_.top_levels(1);
    if (bid.tick == TickLadder::kNoBest || ask.tick == TickLadder::kNoBest || bid.tick >= ask.tick)
        return {};
    return TopOfBook{price_from_tick(bid.tick), bid.quantity, price_from_tick(ask.tick),
                     ask.quantity};
}

L2MarketView L2OrderBook::market_view(std::size_t levels) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    L2MarketView view{};
    view.symbol = symbol_;
    view.ts = timestamp_;
    view.local_ts = local_timestamp_;

    // One bounded walk per side produces the touch and the depth sum together. The previous shape asked
    // nth() for each level separately, which restarted the walk from the best slot every time.
    const TickLadder::TopLevels bid = bids_.top_levels(levels);
    const TickLadder::TopLevels ask = asks_.top_levels(levels);
    if (bid.tick == TickLadder::kNoBest || ask.tick == TickLadder::kNoBest || bid.tick >= ask.tick)
        return view;

    view.top = TopOfBook{price_from_tick(bid.tick), bid.quantity, price_from_tick(ask.tick),
                         ask.quantity};
    view.bid_depth = bid.depth;
    view.ask_depth = ask.depth;

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
    PriceTick tick = 0;
    uint64_t quantity = 0;
    if (side == Side::Buy && bids_.nth(index, tick, quantity))
        return DepthLevel{price_from_tick(tick), quantity};
    if (side == Side::Sell && asks_.nth(index, tick, quantity))
        return DepthLevel{price_from_tick(tick), quantity};
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
