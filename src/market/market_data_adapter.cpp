#include "market/market_data_adapter.h"

#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
std::vector<std::string> split_csv(const std::string& row)
{
    std::vector<std::string> fields;
    std::istringstream stream(row);
    std::string field;
    while (std::getline(stream, field, ',')) fields.push_back(field);
    return fields;
}

uint64_t scaled_quantity(const std::string& value, uint64_t scale)
{
    const double quantity = std::stod(value);
    if (!std::isfinite(quantity) || quantity < 0.0)
        throw std::invalid_argument("quantity must be finite and non-negative");
    return static_cast<uint64_t>(std::llround(quantity * static_cast<double>(scale)));
}

bool parse_boolean(const std::string& value)
{
    if (value == "true" || value == "True" || value == "1") return true;
    if (value == "false" || value == "False" || value == "0") return false;
    throw std::invalid_argument("invalid boolean value: " + value);
}

class BinanceAggTradeReader final : public IMarketEventReader
{
   public:
    BinanceAggTradeReader(const std::string& path, std::string symbol, uint64_t quantity_scale)
        : input_(path), symbol_(std::move(symbol)), quantity_scale_(quantity_scale)
    {
        if (!input_) throw std::runtime_error("failed to open Binance aggTrades file: " + path);
    }

    bool next(MarketEvent& event) override
    {
        std::string row;
        while (std::getline(input_, row))
        {
            if (row.empty()) continue;
            const auto fields = split_csv(row);
            if (!fields.empty() &&
                (fields[0] == "agg_trade_id" || fields[0] == "aggregate_trade_id"))
                continue;
            if (fields.size() < 7) throw std::invalid_argument("invalid Binance aggTrades row");

            const bool buyer_is_maker = parse_boolean(fields[6]);
            event = MarketTrade{.ts = std::stoull(fields[5]),
                                .symbol = symbol_,
                                .price = std::stod(fields[1]),
                                .quantity = scaled_quantity(fields[2], quantity_scale_),
                                .aggressor_side = buyer_is_maker ? Side::Sell : Side::Buy,
                                .sequence = std::stoull(fields[0])};
            return true;
        }
        return false;
    }

   private:
    std::ifstream input_;
    std::string symbol_;
    uint64_t quantity_scale_;
};

class BinanceBookTickerReader final : public IMarketEventReader
{
   public:
    BinanceBookTickerReader(const std::string& path, std::string symbol, uint64_t quantity_scale)
        : input_(path), symbol_(std::move(symbol)), quantity_scale_(quantity_scale)
    {
        if (!input_) throw std::runtime_error("failed to open Binance bookTicker file: " + path);
    }

    bool next(MarketEvent& event) override
    {
        std::string row;
        while (std::getline(input_, row))
        {
            if (row.empty() || row.rfind("update_id,", 0) == 0) continue;
            const auto fields = split_csv(row);
            if (fields.size() < 7) throw std::invalid_argument("invalid Binance bookTicker row");

            event = BboQuote{.ts = std::stoull(fields[5]),
                             .symbol = symbol_,
                             .bid_price = std::stod(fields[1]),
                             .bid_quantity = scaled_quantity(fields[2], quantity_scale_),
                             .ask_price = std::stod(fields[3]),
                             .ask_quantity = scaled_quantity(fields[4], quantity_scale_),
                             .sequence = std::stoull(fields[0])};
            return true;
        }
        return false;
    }

   private:
    std::ifstream input_;
    std::string symbol_;
    uint64_t quantity_scale_;
};
}  // namespace

MarketDataReaders make_market_data_readers(const std::string& format,
                                           const std::string& trades_path,
                                           const std::string& quotes_path,
                                           const InstrumentSpec& instrument)
{
    if (format != "binance")
        throw std::invalid_argument("unsupported market data format: " + format);
    if (instrument.symbol.empty())
        throw std::invalid_argument("market data symbol cannot be empty");
    if (instrument.quantity_scale == 0)
        throw std::invalid_argument("quantity scale must be positive");

    return MarketDataReaders{std::make_unique<BinanceAggTradeReader>(trades_path, instrument.symbol,
                                                                     instrument.quantity_scale),
                             std::make_unique<BinanceBookTickerReader>(
                                 quotes_path, instrument.symbol, instrument.quantity_scale)};
}
