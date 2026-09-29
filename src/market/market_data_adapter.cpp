#include "market/market_data_adapter.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "market/line_reader.h"

namespace
{
// The cold path is a separate function so the hot loops below are not carrying the exception
// machinery, the std::string construction and the throw as inlined code on every row.
[[noreturn]] void throw_invalid_uint(std::string_view value)
{
    throw std::invalid_argument("invalid unsigned integer: " + std::string(value));
}

uint64_t parse_uint64(std::string_view value)
{
    // Timestamps and sizes in this feed are well under 19 digits, so the two-digit step below needs no
    // overflow check: a value of at most 18 digits cannot exceed 2^64-1. For that shape the loop
    // validates every character it consumes, so it accepts exactly what std::from_chars accepts and
    // produces the same value. Anything longer — or empty — goes through the general parser, which
    // keeps the previous behaviour for lengths that could overflow.
    constexpr std::size_t kMaxFastDigits = 18;
    const char* p = value.data();
    const char* const end = p + value.size();
    const std::size_t length = value.size();
    if (length == 0 || length > kMaxFastDigits)
    {
        uint64_t general = 0;
        const auto parsed = std::from_chars(p, end, general);
        if (parsed.ec != std::errc{} || parsed.ptr != end) throw_invalid_uint(value);
        return general;
    }

    uint64_t result = 0;
    if ((length & 1U) != 0)
    {
        const unsigned digit = static_cast<unsigned>(*p) - '0';
        if (digit > 9) throw_invalid_uint(value);
        result = digit;
        ++p;
    }
    while (p != end)
    {
        const unsigned high = static_cast<unsigned>(p[0]) - '0';
        const unsigned low = static_cast<unsigned>(p[1]) - '0';
        if (high > 9 || low > 9) throw_invalid_uint(value);
        result = result * 100 + high * 10 + low;
        p += 2;
    }
    return result;
}

double parse_double_slow(std::string_view value)
{
    double result = 0.0;
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result, std::chars_format::general);
    if (parsed.ec == std::errc{} && parsed.ptr == end) return result;
    return std::stod(std::string(value));
}

// Prices in this feed are plain decimals, and the book works in integer ticks, so the conversion can
// be done with integer arithmetic instead of the general purpose parser. std::from_chars<double> is
// defined out of line in libstdc++, which makes it a real call per row; the loops below avoid it for
// the shape the feed actually uses.
//
// The fast path only accepts what it can reproduce exactly: an optional sign, at least one integer
// digit, an optional fraction of at most five digits, and a mantissa that a double holds exactly. For
// that shape the value is mantissa / 10^decimals, and because both operands are exact and IEEE
// division is correctly rounded, the result is the correctly rounded value of the exact decimal,
// which is what std::from_chars produces. Everything else falls back to the general parser, so the
// returned value is identical for every input.
double parse_double(std::string_view value)
{
    constexpr std::int64_t kMaxExactMantissa = (std::int64_t{1} << 53) - 1;
    constexpr int kMaxFastDecimals = 5;
    static constexpr double kPow10[] = {1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0};

    std::int64_t mantissa = 0;
    int decimals = 0;
    std::size_t index = 0;
    bool negative = false;
    if (index < value.size() && (value[index] == '-' || value[index] == '+'))
    {
        negative = value[index] == '-';
        ++index;
    }
    const std::size_t digits_begin = index;
    while (index < value.size() && value[index] >= '0' && value[index] <= '9')
    {
        mantissa = mantissa * 10 + (value[index] - '0');
        if (mantissa > kMaxExactMantissa) return parse_double_slow(value);
        ++index;
    }
    if (index == digits_begin) return parse_double_slow(value);
    if (index < value.size() && value[index] == '.')
    {
        ++index;
        const std::size_t fraction_begin = index;
        while (index < value.size() && value[index] >= '0' && value[index] <= '9')
        {
            if (decimals == kMaxFastDecimals) return parse_double_slow(value);
            mantissa = mantissa * 10 + (value[index] - '0');
            if (mantissa > kMaxExactMantissa) return parse_double_slow(value);
            ++decimals;
            ++index;
        }
        // A trailing '.' is accepted by std::stod but not by the integer path, so let it fall back.
        if (index == fraction_begin) return parse_double_slow(value);
    }
    // Anything left over (an exponent, a stray character) is not this fast path's business.
    if (index != value.size()) return parse_double_slow(value);

    const double magnitude = static_cast<double>(mantissa) / kPow10[decimals];
    return negative ? -magnitude : magnitude;
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
    // First character dispatch. It runs once per row, and the set of accepted spellings is
    // identical to the chain it replaces: only the order of the comparisons changes. A first
    // character of 'f' goes straight to the false spellings instead of testing the true ones
    // first.
    if (!value.empty())
    {
        switch (value.front())
        {
            case 't':
            case 'T':
                if (value == "true" || value == "TRUE") return true;
                break;
            case 'f':
            case 'F':
                if (value == "false" || value == "FALSE") return false;
                break;
            case '1':
                if (value == "1") return true;
                break;
            case '0':
                if (value == "0") return false;
                break;
            default:
                break;
        }
    }

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
