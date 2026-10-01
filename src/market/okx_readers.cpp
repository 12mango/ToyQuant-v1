// The OKX readers: the daily order-book archive, which is JSON Lines, and the traded CSV that
// tools/fetch_okx_trades.py writes out of the same archive. Both take the contract size and the quantity
// scale because an OKX swap quotes a number of contracts rather than a size in the base asset.

#include "market/market_data_adapter.h"

#include <cstdint>
#include <memory>
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
using csv_fields::parse_double;
using csv_fields::parse_uint64;

// OKX publishes its order book archive as JSON Lines: one object per line carrying instId, action, ts and
// both sides as arrays of [price, size, orders]. The first line of a file is a full depth snapshot and the
// rest are updates that list only the levels that changed, where a size of zero removes a level. Sizes are
// contract counts, so a linear swap of contract value 0.01 is multiplied by that value and the resulting
// quantity unit is the base asset, exactly like a spot style instrument, which is why the existing fee and
// position arithmetic needs no new field for this venue.
//
// Two properties of this feed are visible here rather than in a document. There is no sequence number of any
// kind, so a missing record cannot be detected from the file itself. There is also no local receive timestamp,
// so the exchange timestamp fills both stamp fields of a batch, which is what the batch metadata requires.

[[nodiscard]] std::string_view json_string(std::string_view object, std::string_view quoted_key,
                                           std::string_view what)
{
    const std::size_t begin = object.find(quoted_key);
    if (begin == std::string_view::npos)
        throw std::invalid_argument("okx line is missing " + std::string(what));
    const std::size_t value_begin = begin + quoted_key.size();
    const std::size_t value_end = object.find('"', value_begin);
    if (value_end == std::string_view::npos)
        throw std::invalid_argument("unterminated okx string value for " + std::string(what));
    return object.substr(value_begin, value_end - value_begin);
}

struct OkxLevel
{
    double price{};
    double size{};
};

// Levels arrive as [["price","size","orders"],...]. Both numbers are quoted in this feed, only the first two
// fields are read, and the parser refuses any shape it does not recognise instead of returning a book that is
// quietly wrong.
[[nodiscard]] std::string_view unquoted(std::string_view value)
{
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        return value.substr(1, value.size() - 2);
    return value;
}

void parse_okx_levels(std::string_view object, std::string_view quoted_key, std::vector<OkxLevel>& into,
                      std::string_view what)
{
    into.clear();
    const std::size_t key = object.find(quoted_key);
    if (key == std::string_view::npos)
        throw std::invalid_argument("okx line is missing " + std::string(what));
    std::size_t cursor = object.find('[', key);
    if (cursor == std::string_view::npos)
        throw std::invalid_argument("okx " + std::string(what) + " is not an array");
    ++cursor;
    while (cursor < object.size() && object[cursor] != ']')
    {
        if (object[cursor] == ',')
        {
            ++cursor;
            continue;
        }
        if (object[cursor] != '[')
            throw std::invalid_argument("okx " + std::string(what) + " holds a non array level");
        const std::size_t level_end = object.find(']', cursor);
        if (level_end == std::string_view::npos)
            throw std::invalid_argument("unterminated okx level in " + std::string(what));
        const std::string_view level = object.substr(cursor + 1, level_end - cursor - 1);
        const std::size_t price_end = level.find(',');
        const std::size_t size_end =
            price_end == std::string_view::npos ? std::string_view::npos : level.find(',', price_end + 1);
        if (price_end == std::string_view::npos || size_end == std::string_view::npos)
            throw std::invalid_argument("okx level needs price and size");
        into.push_back(OkxLevel{parse_double(unquoted(level.substr(0, price_end))),
                                parse_double(unquoted(level.substr(price_end + 1, size_end - price_end - 1)))});
        cursor = level_end + 1;
    }
}

class OkxBookJsonReader final : public IMarketEventReader
{
   public:
    OkxBookJsonReader(const std::string& path, std::string symbol, double contract_size,
                      uint64_t quantity_scale)
        : reader_(path),
          symbol_(std::move(symbol)),
          contract_size_(contract_size),
          quantity_scale_(quantity_scale)
    {
        if (contract_size_ <= 0.0) throw std::invalid_argument("okx contract size must be positive");
        if (quantity_scale_ == 0) throw std::invalid_argument("okx quantity scale must be positive");
    }

    bool next(MarketEvent& event) override
    {
        while (true)
        {
            if (!have_current_ && !pull()) return false;
            have_current_ = false;
            if (current_.instrument != symbol_) continue;
            if (current_.action == "snapshot")
            {
                event = make_snapshot();
                return true;
            }
            if (current_.action != "update")
                throw std::invalid_argument("unknown okx book action: " + std::string(current_.action));
            event = make_batch();
            return true;
        }
    }

   private:
    // One parsed line, with the two vectors reused across lines so a line costs no allocation once warm.
    struct Line
    {
        std::string_view instrument;
        std::string_view action;
        uint64_t ts{};
        std::vector<OkxLevel> bids;
        std::vector<OkxLevel> asks;
    };

    bool pull()
    {
        std::string_view row;
        while (reader_.next_line(row))
        {
            if (row.empty()) continue;
            current_.instrument = json_string(row, "\"instId\":\"", "instId");
            current_.action = json_string(row, "\"action\":\"", "action");
            current_.ts = parse_uint64(json_string(row, "\"ts\":\"", "ts"));
            parse_okx_levels(row, "\"bids\":", current_.bids, "bids");
            parse_okx_levels(row, "\"asks\":", current_.asks, "asks");
            return true;
        }
        return false;
    }

    // A size of zero is how this feed removes a level, so it maps to the zero amount the book removes on.
    [[nodiscard]] uint64_t amount_of(double size) const
    {
        if (size <= 0.0) return 0;
        return static_cast<uint64_t>(
            std::llround(size * contract_size_ * static_cast<double>(quantity_scale_)));
    }

    MarketEvent make_snapshot()
    {
        // The book carries a counter it increments per batch, and this feed has no sequence numbers at all, so
        // the snapshot's ordering token is its timestamp: that is always larger than the counter and it moves
        // forward across the archive's periodic re-snapshots, which is exactly what the book checks.
        MarketDepthSnapshot snapshot{.ts = current_.ts,
                                     .symbol = std::string(current_.instrument),
                                     .sequence = current_.ts,
                                     .bids = {},
                                     .asks = {},
                                     .exchange = "okx"};
        snapshot.bids.reserve(current_.bids.size());
        snapshot.asks.reserve(current_.asks.size());
        for (const auto& level : current_.bids)
            snapshot.bids.push_back(DepthLevel{level.price, amount_of(level.size)});
        for (const auto& level : current_.asks)
            snapshot.asks.push_back(DepthLevel{level.price, amount_of(level.size)});
        return snapshot;
    }

    void append_updates(IncrementalBookBatch& batch, const Line& line) const
    {
        for (const auto& level : line.bids)
            batch.updates.push_back(IncrementalBookUpdate{.exchange_ts = line.ts,
                                                          .local_ts = line.ts,
                                                          .is_snapshot = false,
                                                          .side = Side::Buy,
                                                          .price = level.price,
                                                          .amount = amount_of(level.size)});
        for (const auto& level : line.asks)
            batch.updates.push_back(IncrementalBookUpdate{.exchange_ts = line.ts,
                                                          .local_ts = line.ts,
                                                          .is_snapshot = false,
                                                          .side = Side::Sell,
                                                          .price = level.price,
                                                          .amount = amount_of(level.size)});
    }

    MarketEvent make_batch()
    {
        // The stamps come from the one timestamp the file carries, because the archive has no local receive
        // time and the batch metadata requires both fields.
        IncrementalBookBatch batch{.ts = current_.ts,
                                   .exchange_ts = current_.ts,
                                   .local_ts = current_.ts,
                                   .symbol = std::string(current_.instrument),
                                   .updates = {},
                                   .exchange = "okx",
                                   .snapshot_metadata_valid = true,
                                   .contains_snapshot = false};
        append_updates(batch, current_);
        // Several level changes share a timestamp in this feed, so a batch ends where the next line's
        // timestamp differs, which is how the incremental reader batches its rows too.
        while (true)
        {
            if (!have_current_)
            {
                have_current_ = pull();
                if (have_current_ && current_.ts != batch.ts) break;
                if (!have_current_) break;
            }
            append_updates(batch, current_);
            have_current_ = false;
        }
        return batch;
    }

    LineReader reader_;
    std::string symbol_;
    double contract_size_;
    uint64_t quantity_scale_;
    Line current_;
    bool have_current_{false};
    uint64_t sequence_{};
};

// A linear swap quotes a size in contracts, so the base amount is size times the contract value. Both the
// book reader and the trade reader use this, so the two cannot disagree about what a size means.
[[nodiscard]] uint64_t contract_amount(double size, double contract_size, uint64_t quantity_scale)
{
    if (size <= 0.0) return 0;
    return static_cast<uint64_t>(
        std::llround(size * contract_size * static_cast<double>(quantity_scale)));
}

class OkxTradeCsvReader final : public IMarketEventReader
{
   public:
    OkxTradeCsvReader(const std::string& path, std::string symbol, double contract_size,
                      uint64_t quantity_scale)
        : reader_(path),
          symbol_(std::move(symbol)),
          contract_size_(contract_size),
          quantity_scale_(quantity_scale)
    {
        std::string_view header;
        if (!reader_.next_line(header)) throw std::invalid_argument("okx trades file is missing a header");
        std::vector<std::string_view> fields;
        split_csv_views(header, fields, 0);
        if (fields.size() < kColumns || fields[0] != "instrument_name" || fields[5] != "created_time")
            throw std::invalid_argument("unexpected okx trades columns");
    }

    bool next(MarketEvent& event) override
    {
        std::string_view row;
        while (reader_.next_line(row))
        {
            if (row.empty()) continue;
            split_csv_views(row, fields_, kColumns);
            if (fields_.size() < kColumns) throw std::invalid_argument("invalid okx trade row");
            if (fields_[0] != symbol_) continue;
            const std::string_view side = fields_[2];
            if (side != "buy" && side != "sell")
                throw std::invalid_argument("unknown okx trade side: " + std::string(side));
            event = MarketTrade{.ts = parse_uint64(fields_[5]),
                                .symbol = symbol_,
                                .price = parse_double(fields_[3]),
                                .quantity = contract_amount(parse_double(fields_[4]), contract_size_,
                                                            quantity_scale_),
                                .aggressor_side = side == "buy" ? Side::Buy : Side::Sell,
                                .sequence = parse_uint64(fields_[1]),
                                .exchange = "okx"};
            return true;
        }
        return false;
    }

   private:
    // instrument_name, trade_id, side, price, size, created_time. The side is the aggressor's.
    static constexpr std::size_t kColumns = 6;

    LineReader reader_;
    std::string symbol_;
    double contract_size_;
    uint64_t quantity_scale_;
    std::vector<std::string_view> fields_;
};
}  // namespace

std::unique_ptr<IMarketEventReader> make_okx_book_reader(const std::string& path, const std::string& symbol,
                                                         double contract_size, uint64_t quantity_scale)
{
    return std::make_unique<OkxBookJsonReader>(path, symbol, contract_size, quantity_scale);
}

std::unique_ptr<IMarketEventReader> make_okx_trade_reader(const std::string& path, const std::string& symbol,
                                                          double contract_size, uint64_t quantity_scale)
{
    return std::make_unique<OkxTradeCsvReader>(path, symbol, contract_size, quantity_scale);
}
