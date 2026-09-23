#include "market/market_data_adapter.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <zlib.h>

namespace
{
std::vector<std::string> split_csv(const std::string& row)
{
    std::vector<std::string> fields;
    std::istringstream stream(row);
    std::string field;
    while (std::getline(stream, field, ','))
    {
        if (!field.empty() && field.back() == '\r') field.pop_back();
        if (fields.empty() && field.size() >= 3 &&
            static_cast<unsigned char>(field[0]) == 0xEF &&
            static_cast<unsigned char>(field[1]) == 0xBB &&
            static_cast<unsigned char>(field[2]) == 0xBF)
            field.erase(0, 3);
        fields.push_back(field);
    }
    return fields;
}

uint64_t scaled_quantity(const std::string& value, uint64_t scale)
{
    const double quantity = std::stod(value);
    if (!std::isfinite(quantity) || quantity < 0.0)
        throw std::invalid_argument("quantity must be finite and non-negative");
    return static_cast<uint64_t>(std::llround(quantity * static_cast<double>(scale)));
}

uint64_t snapshot_quantity(const std::string& value)
{
    const double quantity = std::stod(value);
    if (!std::isfinite(quantity) || quantity < 0.0 ||
        quantity > static_cast<double>(std::numeric_limits<uint64_t>::max()))
        throw std::invalid_argument("snapshot quantity must be finite and non-negative");
    return static_cast<uint64_t>(std::llround(quantity));
}

bool parse_boolean(const std::string& value)
{
    std::string normalized = value;
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
                                    [](unsigned char character)
                                    { return std::isspace(character) || character == '"'; }),
                     normalized.end());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (normalized == "true" || normalized == "1") return true;
    if (normalized == "false" || normalized == "0") return false;
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
                                .sequence = std::stoull(fields[0]),
                                .exchange = "binance"};
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
                             .sequence = std::stoull(fields[0]),
                             .exchange = "binance"};
            return true;
        }
        return false;
    }

   private:
    std::ifstream input_;
    std::string symbol_;
    uint64_t quantity_scale_;
};

class DeribitBookSnapshotReader final : public IMarketEventReader
{
   public:
    explicit DeribitBookSnapshotReader(const std::string& path) : input_(path)
    {
        if (!input_) throw std::runtime_error("failed to open Deribit book snapshot: " + path);

        std::string header;
        if (!std::getline(input_, header))
            throw std::invalid_argument("Deribit book snapshot is missing a header");
        const auto fields = split_csv(header);
        for (std::size_t index = 0; index < fields.size(); ++index)
            columns_.emplace(fields[index], index);

        symbol_column_ = require_column("symbol");
        timestamp_column_ = require_column("timestamp");
        exchange_column_ = require_column("exchange");
        for (std::size_t level = 0;; ++level)
        {
            const auto bid_price = find_column("bids[" + std::to_string(level) + "].price");
            const auto bid_amount = find_column("bids[" + std::to_string(level) + "].amount");
            const auto ask_price = find_column("asks[" + std::to_string(level) + "].price");
            const auto ask_amount = find_column("asks[" + std::to_string(level) + "].amount");
            if (!bid_price && !bid_amount && !ask_price && !ask_amount) break;
            if (!bid_price || !bid_amount || !ask_price || !ask_amount)
                throw std::invalid_argument("incomplete depth level: " + std::to_string(level));
            bids_.push_back({*bid_price, *bid_amount});
            asks_.push_back({*ask_price, *ask_amount});
        }
        if (bids_.empty()) throw std::invalid_argument("Deribit book snapshot has no depth levels");
    }

    bool next(MarketEvent& event) override
    {
        std::string row;
        while (std::getline(input_, row))
        {
            if (row.empty()) continue;
            const auto fields = split_csv(row);
            if (fields.size() < columns_.size())
                throw std::invalid_argument("invalid Deribit book snapshot row");

            MarketDepthSnapshot snapshot{.ts = std::stoull(fields[timestamp_column_]),
                                         .symbol = fields[symbol_column_],
                                         .sequence = ++sequence_,
                                         .exchange = fields[exchange_column_]};
            for (const auto& [price_column, quantity_column] : bids_)
                snapshot.bids.push_back(
                    DepthLevel{std::stod(fields[price_column]),
                               snapshot_quantity(fields[quantity_column])});
            for (const auto& [price_column, quantity_column] : asks_)
                snapshot.asks.push_back(
                    DepthLevel{std::stod(fields[price_column]),
                               snapshot_quantity(fields[quantity_column])});
            event = std::move(snapshot);
            return true;
        }
        return false;
    }

   private:
    std::optional<std::size_t> find_column(const std::string& name) const
    {
        const auto column = columns_.find(name);
        if (column == columns_.end()) return std::nullopt;
        return column->second;
    }

    std::size_t require_column(const std::string& name) const
    {
        const auto column = columns_.find(name);
        if (column == columns_.end()) throw std::invalid_argument("missing snapshot column: " + name);
        return column->second;
    }

    std::ifstream input_;
    std::unordered_map<std::string, std::size_t> columns_;
    std::size_t symbol_column_{};
    std::size_t timestamp_column_{};
    std::size_t exchange_column_{};
    std::vector<std::pair<std::size_t, std::size_t>> bids_;
    std::vector<std::pair<std::size_t, std::size_t>> asks_;
    uint64_t sequence_{};
};

class DeribitTradeReader final : public IMarketEventReader
{
   public:
    explicit DeribitTradeReader(const std::string& path)
    {
        if (path.size() >= 3 && path.substr(path.size() - 3) == ".gz")
        {
            gzip_input_ = gzopen(path.c_str(), "rb");
            if (!gzip_input_) throw std::runtime_error("failed to open Deribit trade file: " + path);
        }
        else
        {
            plain_input_.open(path);
            if (!plain_input_)
                throw std::runtime_error("failed to open Deribit trade file: " + path);
        }

        std::string header;
        if (!read_line(header)) throw std::invalid_argument("Deribit trade file is missing a header");
        const auto fields = split_csv(header);
        for (std::size_t index = 0; index < fields.size(); ++index) columns_.emplace(fields[index], index);
        exchange_column_ = require_column("exchange");
        symbol_column_ = require_column("symbol");
        timestamp_column_ = require_column("timestamp");
        id_column_ = require_column("id");
        side_column_ = require_column("side");
        price_column_ = require_column("price");
        amount_column_ = require_column("amount");
    }

    ~DeribitTradeReader() override
    {
        if (gzip_input_) gzclose(gzip_input_);
    }

    bool next(MarketEvent& event) override
    {
        std::string row;
        while (read_line(row))
        {
            if (row.empty()) continue;
            const auto fields = split_csv(row);
            if (fields.size() < columns_.size())
                throw std::invalid_argument("invalid Deribit trade row");

            const auto& side = fields[side_column_];
            const Side aggressor_side = side == "buy"   ? Side::Buy
                                         : side == "sell" ? Side::Sell
                                                           : Side::Unknown;
            if (aggressor_side == Side::Unknown)
                throw std::invalid_argument("invalid Deribit trade side: " + side);

            event = MarketTrade{.ts = std::stoull(fields[timestamp_column_]),
                                .symbol = fields[symbol_column_],
                                .price = std::stod(fields[price_column_]),
                                .quantity = snapshot_quantity(fields[amount_column_]),
                                .aggressor_side = aggressor_side,
                                .sequence = std::stoull(fields[id_column_]),
                                .exchange = fields[exchange_column_]};
            return true;
        }
        return false;
    }

   private:
    bool read_line(std::string& line)
    {
        line.clear();
        if (gzip_input_)
        {
            char buffer[4096];
            while (gzgets(gzip_input_, buffer, sizeof(buffer)))
            {
                line += buffer;
                if (!line.empty() && line.back() == '\n') break;
            }
            if (line.empty()) return false;
        }
        else if (!std::getline(plain_input_, line))
        {
            return false;
        }
        if (!line.empty() && line.back() == '\n') line.pop_back();
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    }

    std::size_t require_column(const std::string& name) const
    {
        const auto column = columns_.find(name);
        if (column == columns_.end()) throw std::invalid_argument("missing trade column: " + name);
        return column->second;
    }

    std::ifstream plain_input_;
    gzFile gzip_input_{nullptr};
    std::unordered_map<std::string, std::size_t> columns_;
    std::size_t exchange_column_{};
    std::size_t symbol_column_{};
    std::size_t timestamp_column_{};
    std::size_t id_column_{};
    std::size_t side_column_{};
    std::size_t price_column_{};
    std::size_t amount_column_{};
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

std::unique_ptr<IMarketEventReader> make_deribit_book_snapshot_reader(const std::string& path)
{
    return std::make_unique<DeribitBookSnapshotReader>(path);
}

std::unique_ptr<IMarketEventReader> make_deribit_trade_reader(const std::string& path)
{
    return std::make_unique<DeribitTradeReader>(path);
}
