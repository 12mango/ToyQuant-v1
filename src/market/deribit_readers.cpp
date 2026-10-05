// The Deribit readers: a whole-book snapshot, the incremental batches that follow it, and the trade tape.
// `make_deribit_depth_reader` is the one entry point that has to look at the file to decide: both depth
// shapes are CSV with a header, and only the incremental one carries an `is_snapshot` column.

#include "market/market_data_adapter.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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
using csv_fields::snapshot_quantity;

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

    [[nodiscard]] bool next(MarketEvent& event) override
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

        // Resolve each column position to its role once, here. The row loop can then dispatch
        // through a jump table instead of walking a chain of eight runtime comparisons against
        // member indices for every field of every row.
        column_roles_.assign(columns_.size(), Column::Ignore);
        column_roles_[exchange_column_] = Column::Exchange;
        column_roles_[symbol_column_] = Column::Symbol;
        column_roles_[timestamp_column_] = Column::Timestamp;
        column_roles_[local_timestamp_column_] = Column::LocalTimestamp;
        column_roles_[snapshot_column_] = Column::Snapshot;
        column_roles_[side_column_] = Column::Side;
        column_roles_[price_column_] = Column::Price;
        column_roles_[amount_column_] = Column::Amount;
    }

    [[nodiscard]] bool next(MarketEvent& event) override
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
    // Role of a column position within one row. Resolved from the header once, so the row loop
    // can switch on it instead of comparing against each column index in turn.
    enum class Column : std::uint8_t
    {
        Ignore = 0,
        Exchange,
        Symbol,
        Timestamp,
        LocalTimestamp,
        Snapshot,
        Side,
        Price,
        Amount
    };

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
            switch (field_index < column_roles_.size() ? column_roles_[field_index]
                                                       : Column::Ignore)
            {
                case Column::Exchange: parsed.exchange = field; break;
                case Column::Symbol: parsed.symbol = field; break;
                case Column::Timestamp: parsed.exchange_ts = parse_uint64(field); break;
                case Column::LocalTimestamp: parsed.local_ts = parse_uint64(field); break;
                case Column::Snapshot: parsed.is_snapshot = parse_boolean(field); break;
                case Column::Side:
                    parsed.side = field == "bid"   ? Side::Buy
                                  : field == "ask" ? Side::Sell
                                                   : Side::Unknown;
                    break;
                case Column::Price: parsed.price = parse_double(field); break;
                case Column::Amount: parsed.amount = snapshot_quantity(field); break;
                case Column::Ignore: break;
            }
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
    std::vector<Column> column_roles_;
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

    [[nodiscard]] bool next(MarketEvent& event) override
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
