#include "market/market_event_recorder.h"

#include <charconv>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace
{
// Prices are written as the shortest decimal that parses back to the same double, and quantities as
// integers, so a recording loses nothing to formatting. to_chars is used rather than a stream
// because it is locale independent: under a locale whose decimal point is a comma the file would be
// split in the wrong place by this project's own readers.
void append_unsigned(std::string& out, uint64_t value)
{
    char buffer[20];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

void append_price(std::string& out, double value)
{
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) throw std::runtime_error("a price could not be written");
    out.append(buffer, result.ptr);
}

void require_writable(std::string_view value, const char* what)
{
    if (!replay_field_is_writable(value))
        throw std::invalid_argument(std::string("recording refused: the ") + what + " field '" +
                                    std::string(value) +
                                    "' holds a character the replay CSV cannot carry");
}
}  // namespace

bool replay_field_is_writable(std::string_view value)
{
    return value.find_first_of(",\"\n\r") == std::string_view::npos;
}

std::string RecordingSummary::to_log_string() const
{
    return "trades=" + std::to_string(trades) + " depth_batches=" + std::to_string(depth_batches) +
           " depth_updates=" + std::to_string(depth_updates) +
           " depth_snapshots=" + std::to_string(depth_snapshots) +
           " depth_levels=" + std::to_string(depth_levels);
}

MarketEventRecorder::MarketEventRecorder(std::string prefix) : prefix_(std::move(prefix))
{
    if (prefix_.empty()) throw std::invalid_argument("a recording needs a file prefix");
    trades_path_ = prefix_ + "_trades.csv";
    depth_path_ = prefix_ + "_depth.csv";

    // The prefix is usually data/v2/... or out/lat/..., and the second of those is normally empty:
    // a recorder that failed on a missing directory would be asking the caller for housekeeping on
    // a file it is about to create anyway.
    const std::filesystem::path parent = std::filesystem::path(trades_path_).parent_path();
    if (!parent.empty())
    {
        std::error_code error;
        std::filesystem::create_directories(parent, error);
        if (error)
            throw std::runtime_error("failed to create the recording directory '" + parent.string() +
                                     "': " + error.message());
    }

    trades_out_.open(trades_path_, std::ios::out | std::ios::trunc);
    depth_out_.open(depth_path_, std::ios::out | std::ios::trunc);
    if (!trades_out_.is_open() || !depth_out_.is_open())
        throw std::runtime_error("failed to open the recording files '" + trades_path_ + "' and '" +
                                 depth_path_ + "'");
    // setbuf has to happen before the first write, so both buffers are installed here even though
    // the depth file's header waits for the first depth event to say which shape it is.
    trades_out_.rdbuf()->pubsetbuf(trades_buffer_.data(),
                                   static_cast<std::streamsize>(trades_buffer_.size()));
    depth_out_.rdbuf()->pubsetbuf(depth_buffer_.data(),
                                  static_cast<std::streamsize>(depth_buffer_.size()));
    trades_out_ << "exchange,symbol,timestamp,id,side,price,amount\n";
}

void MarketEventRecorder::write_row(std::ofstream& out)
{
    out.write(row_.data(), static_cast<std::streamsize>(row_.size()));
    if (!out) throw std::runtime_error("writing the recording failed; the disk may be full");
}

void MarketEventRecorder::record(const MarketEvent& event)
{
    std::visit(
        [this](const auto& value)
        {
            using Event = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Event, MarketTrade>)
                write_trade(value);
            else if constexpr (std::is_same_v<Event, IncrementalBookBatch>)
                write_batch(value);
            else if constexpr (std::is_same_v<Event, MarketDepthSnapshot>)
                write_snapshot(value);
            else
                throw std::invalid_argument(
                    "recording refused: the replay formats this project reads carry a trade stream and "
                    "a depth stream, and a BBO quote is neither, so it has nowhere to go");
        },
        event);
}

void MarketEventRecorder::write_trade(const MarketTrade& trade)
{
    // The trade reader rejects any side it cannot name, so a trade without an aggressor cannot be
    // written as anything that reads back.
    if (trade.aggressor_side != Side::Buy && trade.aggressor_side != Side::Sell)
        throw std::invalid_argument(
            "recording refused: a trade with an unknown aggressor side has no spelling in the replay "
            "trade format");
    require_writable(trade.exchange, "exchange");
    require_writable(trade.symbol, "symbol");

    row_.clear();
    row_.append(trade.exchange);
    row_.push_back(',');
    row_.append(trade.symbol);
    row_.push_back(',');
    append_unsigned(row_, trade.ts);
    row_.push_back(',');
    append_unsigned(row_, trade.sequence);
    row_.append(trade.aggressor_side == Side::Buy ? ",buy," : ",sell,");
    append_price(row_, trade.price);
    row_.push_back(',');
    append_unsigned(row_, trade.quantity);
    row_.push_back('\n');
    write_row(trades_out_);
    ++summary_.trades;
}

void MarketEventRecorder::write_incremental_header()
{
    depth_out_ << "exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount\n";
    depth_shape_ = DepthShape::Incremental;
}

void MarketEventRecorder::write_snapshot_header(std::size_t level_count)
{
    depth_out_ << "exchange,symbol,timestamp";
    for (std::size_t level = 0; level < level_count; ++level)
        depth_out_ << ",asks[" << level << "].price,asks[" << level << "].amount,bids[" << level
                   << "].price,bids[" << level << "].amount";
    depth_out_ << '\n';
    depth_shape_ = DepthShape::Snapshot;
    summary_.depth_levels = level_count;
}

void MarketEventRecorder::write_batch(const IncrementalBookBatch& batch)
{
    if (depth_shape_ == DepthShape::Unset) write_incremental_header();
    if (depth_shape_ != DepthShape::Incremental)
        throw std::invalid_argument(
            "recording refused: this stream carries both a whole-book snapshot shape and incremental "
            "batches, and one depth file cannot hold both");
    require_writable(batch.exchange, "exchange");
    require_writable(batch.symbol, "symbol");

    for (const auto& update : batch.updates)
    {
        // The incremental reader regroups rows by local timestamp, so updates that disagree about
        // theirs would come back as more batches than went in.
        if (update.local_ts != batch.local_ts)
            throw std::invalid_argument(
                "recording refused: a batch whose updates carry different local timestamps cannot be "
                "regrouped into the batch it was read as");

        row_.clear();
        row_.append(batch.exchange);
        row_.push_back(',');
        row_.append(batch.symbol);
        row_.push_back(',');
        append_unsigned(row_, update.exchange_ts);
        row_.push_back(',');
        append_unsigned(row_, update.local_ts);
        row_.append(update.is_snapshot ? ",1," : ",0,");
        row_.append(update.side == Side::Buy ? "bid," : update.side == Side::Sell ? "ask," : ",");
        append_price(row_, update.price);
        row_.push_back(',');
        append_unsigned(row_, update.amount);
        row_.push_back('\n');
        write_row(depth_out_);
        ++summary_.depth_updates;
    }
    ++summary_.depth_batches;
}

void MarketEventRecorder::write_snapshot(const MarketDepthSnapshot& snapshot)
{
    // The snapshot reader takes its level count from the header, and a header is written once, so a
    // stream whose depth changes shape has no single file that represents it.
    if (snapshot.bids.size() != snapshot.asks.size())
        throw std::invalid_argument(
            "recording refused: a snapshot whose two sides have different level counts has no single "
            "replay header");
    if (depth_shape_ == DepthShape::Unset)
    {
        if (snapshot.bids.empty())
            throw std::invalid_argument(
                "recording refused: an empty snapshot has no depth levels to give a header");
        write_snapshot_header(snapshot.bids.size());
    }
    if (depth_shape_ != DepthShape::Snapshot || summary_.depth_levels != snapshot.bids.size())
        throw std::invalid_argument(
            "recording refused: this snapshot has " + std::to_string(snapshot.bids.size()) +
            " levels and the recording's header has " + std::to_string(summary_.depth_levels));
    require_writable(snapshot.exchange, "exchange");
    require_writable(snapshot.symbol, "symbol");

    row_.clear();
    row_.append(snapshot.exchange);
    row_.push_back(',');
    row_.append(snapshot.symbol);
    row_.push_back(',');
    append_unsigned(row_, snapshot.ts);
    for (std::size_t level = 0; level < snapshot.bids.size(); ++level)
    {
        row_.push_back(',');
        append_price(row_, snapshot.asks[level].price);
        row_.push_back(',');
        append_unsigned(row_, snapshot.asks[level].quantity);
        row_.push_back(',');
        append_price(row_, snapshot.bids[level].price);
        row_.push_back(',');
        append_unsigned(row_, snapshot.bids[level].quantity);
    }
    row_.push_back('\n');
    write_row(depth_out_);
    ++summary_.depth_snapshots;
}

void MarketEventRecorder::flush()
{
    trades_out_.flush();
    depth_out_.flush();
    if (!trades_out_ || !depth_out_) throw std::runtime_error("flushing the recording failed");
}

