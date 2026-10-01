// The Binance readers: the aggregate-trade CSV and the book-ticker CSV that the public data dumps use.
// Each reader owns one file, parses it row by row and scales the quantities into the instrument's unit, so
// a caller that has an InstrumentSpec gets events it can compare across venues.

#include "market/market_data_adapter.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "market/field_parser.h"
#include "market/line_reader.h"

namespace
{
// The shared field parsers live in their own header. These bring the ones this file uses into its anonymous
// namespace, so the reader bodies below read exactly as they did when every feed family was one file.
using csv_fields::split_csv_views;
using csv_fields::parse_boolean;
using csv_fields::parse_double;
using csv_fields::parse_uint64;
using csv_fields::scaled_quantity;

class BinanceAggTradeReader final : public IMarketEventReader
{
   public:
    BinanceAggTradeReader(const std::string& path, std::string symbol, uint64_t quantity_scale)
        : reader_(path), symbol_(std::move(symbol)), quantity_scale_(quantity_scale)
    {
    }

    bool next(MarketEvent& event) override
    {
        std::string_view row;
        while (reader_.next_line(row))
        {
            if (row.empty()) continue;
            split_csv_views(row, fields_, kColumns);
            if (!fields_.empty() &&
                (fields_[0] == "agg_trade_id" || fields_[0] == "aggregate_trade_id"))
                continue;
            if (fields_.size() < 7) throw std::invalid_argument("invalid Binance aggTrades row");

            const bool buyer_is_maker = parse_boolean(fields_[6]);
            event = MarketTrade{.ts = parse_uint64(fields_[5]),
                                .symbol = symbol_,
                                .price = parse_double(fields_[1]),
                                .quantity = scaled_quantity(fields_[2], quantity_scale_),
                                .aggressor_side = buyer_is_maker ? Side::Sell : Side::Buy,
                                .sequence = parse_uint64(fields_[0]),
                                .exchange = "binance"};
            return true;
        }
        return false;
    }

   private:
    // agg_trade_id, price, quantity, first_trade_id, last_trade_id, transact_time,
    // is_buyer_maker, is_best_match.
    static constexpr std::size_t kColumns = 8;

    LineReader reader_;
    std::vector<std::string_view> fields_;
    std::string symbol_;
    uint64_t quantity_scale_;
};

class BinanceBookTickerReader final : public IMarketEventReader
{
   public:
    BinanceBookTickerReader(const std::string& path, std::string symbol, uint64_t quantity_scale)
        : reader_(path), symbol_(std::move(symbol)), quantity_scale_(quantity_scale)
    {
    }

    bool next(MarketEvent& event) override
    {
        std::string_view row;
        while (reader_.next_line(row))
        {
            if (row.empty() || row.rfind("update_id,", 0) == 0) continue;
            split_csv_views(row, fields_, kColumns);
            if (fields_.size() < 7) throw std::invalid_argument("invalid Binance bookTicker row");

            event = BboQuote{.ts = parse_uint64(fields_[5]),
                             .symbol = symbol_,
                             .bid_price = parse_double(fields_[1]),
                             .bid_quantity = scaled_quantity(fields_[2], quantity_scale_),
                             .ask_price = parse_double(fields_[3]),
                             .ask_quantity = scaled_quantity(fields_[4], quantity_scale_),
                             .sequence = parse_uint64(fields_[0]),
                             .exchange = "binance"};
            return true;
        }
        return false;
    }

   private:
    // update_id, best_bid_price, best_bid_qty, best_ask_price, best_ask_qty,
    // transaction_time, event_time.
    static constexpr std::size_t kColumns = 7;

    LineReader reader_;
    std::vector<std::string_view> fields_;
    std::string symbol_;
    uint64_t quantity_scale_;
};
}  // namespace

std::unique_ptr<IMarketEventReader> make_binance_trade_reader(const std::string& path,
                                                              const std::string& symbol,
                                                              uint64_t quantity_scale)
{
    // The checks the router used to make before constructing either reader, kept here so a factory called
    // directly from a test refuses the same input with the same message.
    if (symbol.empty()) throw std::invalid_argument("market data symbol cannot be empty");
    if (quantity_scale == 0) throw std::invalid_argument("quantity scale must be positive");
    return std::make_unique<BinanceAggTradeReader>(path, symbol, quantity_scale);
}

std::unique_ptr<IMarketEventReader> make_binance_book_reader(const std::string& path,
                                                             const std::string& symbol,
                                                             uint64_t quantity_scale)
{
    if (symbol.empty()) throw std::invalid_argument("market data symbol cannot be empty");
    if (quantity_scale == 0) throw std::invalid_argument("quantity scale must be positive");
    return std::make_unique<BinanceBookTickerReader>(path, symbol, quantity_scale);
}
