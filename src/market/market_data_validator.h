#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "market/market_event.h"

struct MarketDataValidationConfig
{
    uint64_t max_bbo_age_ms{1000};
    double max_trade_deviation_bps{50.0};
};

struct MarketDataValidationSummary
{
    uint64_t events{};
    uint64_t trades{};
    uint64_t quotes{};
    uint64_t trades_without_bbo{};
    uint64_t stale_trades{};
    uint64_t dislocated_trades{};
    uint64_t max_bbo_age_ms{};
    double max_trade_deviation_bps{};

    bool has_soft_issues() const
    {
        return trades_without_bbo > 0 || stale_trades > 0 || dislocated_trades > 0;
    }
};

class MarketDataValidator
{
   public:
    explicit MarketDataValidator(MarketDataValidationConfig config = {}) : config_(config) {}

    void validate(const MarketEvent& event);
    const MarketDataValidationSummary& summary() const
    {
        return summary_;
    }

   private:
    struct StreamState
    {
        uint64_t timestamp{};
        uint64_t sequence{};
        bool initialized{false};
    };

    void validate_trade(const MarketTrade& trade);
    void validate_quote(const BboQuote& quote);
    static void validate_ordering(StreamState& state, uint64_t timestamp, uint64_t sequence,
                                  const std::string& stream_name);

    MarketDataValidationConfig config_;
    MarketDataValidationSummary summary_;
    uint64_t last_merged_timestamp_{};
    bool has_merged_timestamp_{false};
    std::unordered_map<std::string, StreamState> trade_streams_;
    std::unordered_map<std::string, StreamState> quote_streams_;
    std::unordered_map<std::string, BboQuote> latest_quotes_;
};
