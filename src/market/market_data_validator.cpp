#include "market/market_data_validator.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace
{
void require_positive_finite(double value, const char* field)
{
    if (!std::isfinite(value) || value <= 0.0)
        throw std::invalid_argument(std::string(field) + " must be finite and positive");
}
}  // namespace

void MarketDataValidator::validate_ordering(StreamState& state, uint64_t timestamp,
                                            uint64_t sequence, const std::string& stream_name)
{
    if (timestamp == 0) throw std::invalid_argument(stream_name + " timestamp must be positive");
    if (sequence == 0) throw std::invalid_argument(stream_name + " sequence must be positive");
    if (state.initialized && timestamp < state.timestamp)
        throw std::invalid_argument(stream_name + " timestamp moved backwards");
    if (state.initialized && sequence <= state.sequence)
        throw std::invalid_argument(stream_name + " sequence is not strictly increasing");

    state = StreamState{timestamp, sequence, true};
}

void MarketDataValidator::validate(const MarketEvent& event)
{
    const uint64_t timestamp = std::visit([](const auto& value) { return value.ts; }, event);
    if (has_merged_timestamp_ && timestamp < last_merged_timestamp_)
        throw std::invalid_argument("merged market event timestamp moved backwards");
    last_merged_timestamp_ = timestamp;
    has_merged_timestamp_ = true;
    ++summary_.events;

    std::visit(
        [this](const auto& value)
        {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, MarketTrade>)
                validate_trade(value);
            else if constexpr (std::is_same_v<Event, BboQuote>)
                validate_quote(value);
            else
                validate_depth_snapshot(value);
        },
        event);
}

void MarketDataValidator::validate_trade(const MarketTrade& trade)
{
    validate_ordering(trade_streams_[trade.symbol], trade.ts, trade.sequence,
                      "trade stream '" + trade.symbol + "'");
    if (trade.symbol.empty()) throw std::invalid_argument("trade symbol cannot be empty");
    require_positive_finite(trade.price, "trade price");
    if (trade.quantity == 0) throw std::invalid_argument("trade quantity must be positive");
    if (trade.aggressor_side == Side::Unknown)
        throw std::invalid_argument("trade aggressor side cannot be unknown");
    ++summary_.trades;

    const auto quote_it = latest_quotes_.find(trade.symbol);
    if (quote_it == latest_quotes_.end())
    {
        ++summary_.trades_without_bbo;
        return;
    }

    const BboQuote& quote = quote_it->second;
    const uint64_t age = trade.ts >= quote.ts ? trade.ts - quote.ts : 0;
    summary_.max_bbo_age_ms = std::max(summary_.max_bbo_age_ms, age);
    if (age > config_.max_bbo_age_ms) ++summary_.stale_trades;

    const double mid = (quote.bid_price + quote.ask_price) / 2.0;
    const double deviation_bps = std::abs(trade.price - mid) / mid * 10000.0;
    summary_.max_trade_deviation_bps = std::max(summary_.max_trade_deviation_bps, deviation_bps);
    if (deviation_bps > config_.max_trade_deviation_bps) ++summary_.dislocated_trades;
}

void MarketDataValidator::validate_quote(const BboQuote& quote)
{
    validate_ordering(quote_streams_[quote.symbol], quote.ts, quote.sequence,
                      "BBO stream '" + quote.symbol + "'");
    if (quote.symbol.empty()) throw std::invalid_argument("BBO symbol cannot be empty");
    require_positive_finite(quote.bid_price, "BBO bid price");
    require_positive_finite(quote.ask_price, "BBO ask price");
    if (quote.bid_quantity == 0 || quote.ask_quantity == 0)
        throw std::invalid_argument("BBO quantities must be positive");
    if (quote.bid_price > quote.ask_price)
        throw std::invalid_argument("BBO bid price exceeds ask price");

    ++summary_.quotes;
    latest_quotes_[quote.symbol] = quote;
}

void MarketDataValidator::validate_depth_snapshot(const MarketDepthSnapshot& snapshot)
{
    validate_ordering(depth_streams_[snapshot.symbol], snapshot.ts, snapshot.sequence,
                      "depth stream '" + snapshot.symbol + "'");
    if (snapshot.symbol.empty()) throw std::invalid_argument("depth symbol cannot be empty");
    if (snapshot.bids.empty() || snapshot.asks.empty())
        throw std::invalid_argument("depth snapshot must contain both sides");

    for (std::size_t index = 0; index < snapshot.bids.size(); ++index)
    {
        const auto& level = snapshot.bids[index];
        require_positive_finite(level.price, "depth bid price");
        if (level.quantity == 0) throw std::invalid_argument("depth bid quantity must be positive");
        if (index > 0 && level.price >= snapshot.bids[index - 1].price)
            throw std::invalid_argument("depth bids must be strictly descending");
    }
    for (std::size_t index = 0; index < snapshot.asks.size(); ++index)
    {
        const auto& level = snapshot.asks[index];
        require_positive_finite(level.price, "depth ask price");
        if (level.quantity == 0) throw std::invalid_argument("depth ask quantity must be positive");
        if (index > 0 && level.price <= snapshot.asks[index - 1].price)
            throw std::invalid_argument("depth asks must be strictly ascending");
    }
    if (snapshot.bids.front().price > snapshot.asks.front().price)
        throw std::invalid_argument("depth bid price exceeds ask price");
    ++summary_.depth_snapshots;
}
