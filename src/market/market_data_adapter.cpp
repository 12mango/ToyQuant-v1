#include "market/market_data_adapter.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "market/line_reader.h"

namespace
{
uint64_t parse_uint64(std::string_view value)
{
    uint64_t result = 0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        throw std::invalid_argument("invalid unsigned integer: " + std::string(value));
    return result;
}

double parse_double(std::string_view value)
{
    double result = 0.0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result, std::chars_format::general);
    if (parsed.ec == std::errc{} && parsed.ptr == end) return result;
    return std::stod(std::string(value));
}

void split_csv_views(std::string_view row, std::vector<std::string_view>& fields,
                     std::size_t expected_fields)
{
    fields.clear();
    fields.reserve(expected_fields);
    std::size_t field_start = 0;
    while (field_start < row.size())
    {
        const std::size_t delimiter = row.find(',', field_start);
        std::size_t field_end = delimiter == std::string_view::npos ? row.size() : delimiter;
        if (field_end > field_start && row[field_end - 1] == '\r') --field_end;
        std::size_t value_start = field_start;
        if (fields.empty() && field_end - value_start >= 3 &&
            static_cast<unsigned char>(row[value_start]) == 0xEF &&
            static_cast<unsigned char>(row[value_start + 1]) == 0xBB &&
            static_cast<unsigned char>(row[value_start + 2]) == 0xBF)
            value_start += 3;
        fields.emplace_back(row.data() + value_start, field_end - value_start);
        if (delimiter == std::string_view::npos) break;
        field_start = delimiter + 1;
    }
}

uint64_t scaled_quantity(std::string_view value, uint64_t scale)
{
    const double quantity = parse_double(value);
    if (!std::isfinite(quantity) || quantity < 0.0)
        throw std::invalid_argument("quantity must be finite and non-negative");
    return static_cast<uint64_t>(std::llround(quantity * static_cast<double>(scale)));
}

uint64_t snapshot_quantity(std::string_view value)
{
    const double quantity = parse_double(value);
    if (!std::isfinite(quantity) || quantity < 0.0 ||
        quantity > static_cast<double>(std::numeric_limits<uint64_t>::max()))
        throw std::invalid_argument("snapshot quantity must be finite and non-negative");
    return static_cast<uint64_t>(std::llround(quantity));
}

bool parse_boolean(std::string_view value)
{
    if (value == "true" || value == "TRUE" || value == "1") return true;
    if (value == "false" || value == "FALSE" || value == "0") return false;

    std::string normalized(value);
    normalized.erase(std::remove_if(normalized.begin(), normalized.end(),
                                    [](unsigned char character)
                                    { return std::isspace(character) || character == '"'; }),
                     normalized.end());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (normalized == "true" || normalized == "1") return true;
    if (normalized == "false" || normalized == "0") return false;
    throw std::invalid_argument("invalid boolean value: " + std::string(value));
}

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

class DeribitBookSnapshotReader final : public IMarketEventReader
{
   public:
    explicit DeribitBookSnapshotReader(const std::string& path) : reader_(path)
    {
        std::string_view header;
        if (!reader_.next_line(header))
            throw std::invalid_argument("Deribit book snapshot is missing a header");
        std::vector<std::string_view> header_fields;
        split_csv_views(header, header_fields, 0);
        for (std::size_t index = 0; index < header_fields.size(); ++index)
            columns_.emplace(std::string(header_fields[index]), index);

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
        std::string_view row;
        while (reader_.next_line(row))
        {
            if (row.empty()) continue;
            split_csv_views(row, fields_, columns_.size());
            if (fields_.size() < columns_.size())
                throw std::invalid_argument("invalid Deribit book snapshot row");

            MarketDepthSnapshot snapshot{.ts = parse_uint64(fields_[timestamp_column_]),
                                         .symbol = std::string(fields_[symbol_column_]),
                                         .sequence = ++sequence_,
                                         .bids = {},
                                         .asks = {},
                                         .exchange = std::string(fields_[exchange_column_])};
            snapshot.bids.reserve(bids_.size());
            snapshot.asks.reserve(asks_.size());
            for (const auto& [price_column, quantity_column] : bids_)
                snapshot.bids.push_back(
                    DepthLevel{parse_double(fields_[price_column]),
                               snapshot_quantity(fields_[quantity_column])});
            for (const auto& [price_column, quantity_column] : asks_)
                snapshot.asks.push_back(
                    DepthLevel{parse_double(fields_[price_column]),
                               snapshot_quantity(fields_[quantity_column])});
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

    LineReader reader_;
    std::vector<std::string_view> fields_;
    std::unordered_map<std::string, std::size_t> columns_;
    std::size_t symbol_column_{};
    std::size_t timestamp_column_{};
    std::size_t exchange_column_{};
    std::vector<std::pair<std::size_t, std::size_t>> bids_;
    std::vector<std::pair<std::size_t, std::size_t>> asks_;
    uint64_t sequence_{};
};

class DeribitIncrementalBookReader final : public IMarketEventReader
{
   public:
    explicit DeribitIncrementalBookReader(const std::string& path) : reader_(path)
    {
        std::string_view header;
        if (!reader_.next_line(header))
            throw std::invalid_argument("incremental book is missing a header");
        std::vector<std::string_view> header_fields;
        split_csv_views(header, header_fields, 0);
        for (std::size_t index = 0; index < header_fields.size(); ++index)
            columns_.emplace(std::string(header_fields[index]), index);
        exchange_column_ = require_column("exchange");
        symbol_column_ = require_column("symbol");
        timestamp_column_ = require_column("timestamp");
        local_timestamp_column_ = require_column("local_timestamp");
        snapshot_column_ = require_column("is_snapshot");
        side_column_ = require_column("side");
        price_column_ = require_column("price");
        amount_column_ = require_column("amount");
    }

    bool next(MarketEvent& event) override
    {
        while (true)
        {
            if (!read_batch(event)) return false;
            const auto& batch = std::get<IncrementalBookBatch>(event);
            if (started_ || batch.has_snapshot())
            {
                started_ = true;
                return true;
            }
        }
    }

   private:
    struct ParsedRow
    {
        std::string_view symbol;
        std::string_view exchange;
        uint64_t exchange_ts{};
        uint64_t local_ts{};
        bool is_snapshot{false};
        Side side{Side::Unknown};
        double price{};
        uint64_t amount{};
    };

    ParsedRow parse_row(std::string_view row) const
    {
        ParsedRow parsed;
        std::size_t field_start = 0;
        std::size_t field_index = 0;
        while (field_start < row.size())
        {
            const std::size_t delimiter = row.find(',', field_start);
            std::size_t field_end =
                delimiter == std::string_view::npos ? row.size() : delimiter;
            std::size_t value_start = field_start;
            if (field_index == 0 && field_end - value_start >= 3 &&
                static_cast<unsigned char>(row[value_start]) == 0xEF &&
                static_cast<unsigned char>(row[value_start + 1]) == 0xBB &&
                static_cast<unsigned char>(row[value_start + 2]) == 0xBF)
                value_start += 3;
            const std::string_view field(row.data() + value_start, field_end - value_start);
            if (field_index == exchange_column_) parsed.exchange = field;
            else if (field_index == symbol_column_) parsed.symbol = field;
            else if (field_index == timestamp_column_) parsed.exchange_ts = parse_uint64(field);
            else if (field_index == local_timestamp_column_) parsed.local_ts = parse_uint64(field);
            else if (field_index == snapshot_column_) parsed.is_snapshot = parse_boolean(field);
            else if (field_index == side_column_)
                parsed.side = field == "bid" ? Side::Buy : field == "ask" ? Side::Sell : Side::Unknown;
            else if (field_index == price_column_) parsed.price = parse_double(field);
            else if (field_index == amount_column_) parsed.amount = snapshot_quantity(field);
            if (delimiter == std::string_view::npos) break;
            field_start = delimiter + 1;
            ++field_index;
        }
        if (field_index + 1 < columns_.size())
            throw std::invalid_argument("invalid incremental book row");
        return parsed;
    }

    bool read_batch(MarketEvent& event)
    {
        if (pending_row_.empty())
        {
            std::string_view row;
            do
            {
                if (!reader_.next_line(row)) return false;
            } while (row.empty());
            // The view is only valid until the next refill, so the pending row is copied.
            // assign() reuses the existing capacity, so this does not allocate per batch.
            pending_row_.assign(row.data(), row.size());
        }
        const ParsedRow first = parse_row(pending_row_);
        const uint64_t batch_timestamp = first.exchange_ts;
        const uint64_t local_ts = first.local_ts;
        IncrementalBookBatch batch{.ts = batch_timestamp,
               .exchange_ts = batch_timestamp,
                                   .local_ts = local_ts,
                   .symbol = std::string(first.symbol),
                                   .updates = {},
                   .exchange = std::string(first.exchange),
                                   .snapshot_metadata_valid = true,
                                   .contains_snapshot = false};
        batch.updates.reserve(2);
        append_update(batch, first);
        pending_row_.clear();
        std::string_view row;
        while (reader_.next_line(row))
        {
            if (row.empty()) continue;
            const ParsedRow parsed = parse_row(row);
            if (parsed.local_ts != local_ts)
            {
                pending_row_.assign(row.data(), row.size());
                break;
            }
            append_update(batch, parsed);
        }
        event = std::move(batch);
        return true;
    }

    void append_update(IncrementalBookBatch& batch, const ParsedRow& row)
    {
        if (row.symbol != batch.symbol)
            throw std::invalid_argument("incremental update symbol changed");
        batch.contains_snapshot = batch.contains_snapshot || row.is_snapshot;
        batch.updates.push_back(IncrementalBookUpdate{
            .exchange_ts = row.exchange_ts,
            .local_ts = row.local_ts,
            .is_snapshot = row.is_snapshot,
            .side = row.side,
            .price = row.price,
            .amount = row.amount});
    }

    std::size_t require_column(const std::string& name) const
    {
        const auto column = columns_.find(name);
        if (column == columns_.end())
            throw std::invalid_argument("missing incremental column: " + name);
        return column->second;
    }

    LineReader reader_;
    std::unordered_map<std::string, std::size_t> columns_;
    std::string pending_row_;
    bool started_{false};
    std::size_t exchange_column_{}, symbol_column_{}, timestamp_column_{}, local_timestamp_column_{};
    std::size_t snapshot_column_{}, side_column_{}, price_column_{}, amount_column_{};
};

class DeribitTradeReader final : public IMarketEventReader
{
   public:
    explicit DeribitTradeReader(const std::string& path) : reader_(path)
    {
        std::string_view header;
        if (!reader_.next_line(header))
            throw std::invalid_argument("Deribit trade file is missing a header");
        std::vector<std::string_view> header_fields;
        split_csv_views(header, header_fields, 0);
        for (std::size_t index = 0; index < header_fields.size(); ++index)
            columns_.emplace(std::string(header_fields[index]), index);
        exchange_column_ = require_column("exchange");
        symbol_column_ = require_column("symbol");
        timestamp_column_ = require_column("timestamp");
        id_column_ = require_column("id");
        side_column_ = require_column("side");
        price_column_ = require_column("price");
        amount_column_ = require_column("amount");
    }

    bool next(MarketEvent& event) override
    {
        std::string_view row;
        while (reader_.next_line(row))
        {
            if (row.empty()) continue;
            split_csv_views(row, fields_, columns_.size());
            if (fields_.size() < columns_.size())
                throw std::invalid_argument("invalid Deribit trade row");

            const std::string_view side = fields_[side_column_];
            const Side aggressor_side = side == "buy"   ? Side::Buy
                                         : side == "sell" ? Side::Sell
                                                          : Side::Unknown;
            if (aggressor_side == Side::Unknown)
                throw std::invalid_argument("invalid Deribit trade side: " + std::string(side));

            event = MarketTrade{.ts = parse_uint64(fields_[timestamp_column_]),
                                .symbol = std::string(fields_[symbol_column_]),
                                .price = parse_double(fields_[price_column_]),
                                .quantity = snapshot_quantity(fields_[amount_column_]),
                                .aggressor_side = aggressor_side,
                                .sequence = parse_uint64(fields_[id_column_]),
                                .exchange = std::string(fields_[exchange_column_])};
            return true;
        }
        return false;
    }

   private:
    std::size_t require_column(const std::string& name) const
    {
        const auto column = columns_.find(name);
        if (column == columns_.end()) throw std::invalid_argument("missing trade column: " + name);
        return column->second;
    }

    LineReader reader_;
    std::vector<std::string_view> fields_;
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

std::unique_ptr<IMarketEventReader> make_deribit_incremental_book_reader(const std::string& path)
{
    return std::make_unique<DeribitIncrementalBookReader>(path);
}

std::unique_ptr<IMarketEventReader> make_deribit_depth_reader(const std::string& path)
{
    std::string_view header;
    {
        LineReader probe(path);
        if (!probe.next_line(header)) throw std::invalid_argument("Deribit depth file is missing a header");
        if (header.find("is_snapshot") != std::string_view::npos)
            return make_deribit_incremental_book_reader(path);
    }
    return make_deribit_book_snapshot_reader(path);
}

std::unique_ptr<IMarketEventReader> make_deribit_trade_reader(const std::string& path)
{
    return std::make_unique<DeribitTradeReader>(path);
}
