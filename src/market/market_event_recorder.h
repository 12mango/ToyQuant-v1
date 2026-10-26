#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

#include "common/market_event.h"

/*
-------------------------------------------
 MarketEventRecorder
-------------------------------------------

Writes the events a feed delivered back out in the replay CSV shapes, so a session that was
replayed or paced against a live stream becomes a file that can be replayed again. The formats are
the ones `src/market/market_data_adapter.cpp` already reads, rather than a private one, because the
point of a recording is that nothing new has to be written to read it:

    <prefix>_trades.csv  exchange,symbol,timestamp,id,side,price,amount
    <prefix>_depth.csv   exchange,symbol,timestamp,local_timestamp,is_snapshot,side,price,amount
                         (the wider per-level snapshot shape when the stream carries snapshots)

What makes the two files a record rather than a summary of one:

  * a price is written as the shortest decimal that parses back to the same double, and a quantity
    as an integer, so the reader reconstructs the exact value it delivered;
  * the rows of an incremental batch carry the batch's own exchange and local timestamps, which is
    what the reader groups on, so the batch comes back as one batch;
  * the interleaving of the trade stream and the depth stream is re-derived from the timestamps by
    the same merge rule the replay used the first time.

What it deliberately does not carry: the readers' own running ids for a snapshot or a batch. The
replay formats have no column for one, the readers assign their own, and no decision reads it, so a
recording changes that field and nothing else.

A stream that the replay formats cannot express is refused rather than written approximately: a
trade with no aggressor side, a field holding the comma the format splits on, a batch whose updates
disagree about their local timestamp, or a snapshot whose level count changes mid-file.
*/

// What a recording contains. Every number is checkable against the file it describes: there is one
// row per trade, one row per level change, and one row per level of a snapshot, so `wc -l` and this
// line have to agree.
struct RecordingSummary
{
    uint64_t trades{0};
    uint64_t depth_batches{0};
    uint64_t depth_updates{0};
    uint64_t depth_snapshots{0};
    // Level count of the snapshot shape this recording used, and 0 when the stream carried batches
    // instead. A file's header is fixed before its first row, which is why a snapshot stream whose
    // depth changes shape is refused rather than written.
    uint64_t depth_levels{0};

    std::string to_log_string() const;
};

class MarketEventRecorder
{
   public:
    // A prefix names both files: <prefix>_trades.csv and <prefix>_depth.csv. The directory is
    // created if it does not exist, because a recording is usually written next to the input it
    // came from and that directory may be empty.
    explicit MarketEventRecorder(std::string prefix);

    // The tap itself, which is a feed callback. It never drops an event silently: an event the
    // replay format cannot carry is an error rather than a hole in the file.
    void record(const MarketEvent& event);

    // Pushes both streams out, so a caller can log what was written while still holding the files.
    void flush();

    const RecordingSummary& summary() const
    {
        return summary_;
    }

    const std::string& trades_path() const
    {
        return trades_path_;
    }

    const std::string& depth_path() const
    {
        return depth_path_;
    }

   private:
    void write_trade(const MarketTrade& trade);
    // One row per level change, which is how the incremental reader regroups a batch.
    void write_batch(const IncrementalBookBatch& batch);
    // One row per snapshot, which is a whole book rather than a change to one.
    void write_snapshot(const MarketDepthSnapshot& snapshot);
    void write_incremental_header();
    void write_snapshot_header(std::size_t level_count);
    void write_row(std::ofstream& out);

    std::string prefix_;
    std::string trades_path_;
    std::string depth_path_;
    std::ofstream trades_out_;
    std::ofstream depth_out_;
    // 256 KiB each: the default stream buffer measures in kilobytes, which on a window this
    // repository replays is enough writes to show up in the very stage timings the recording is
    // meant to be read next to.
    static constexpr std::size_t kIoBufferBytes = std::size_t{1} << 18;
    std::array<char, kIoBufferBytes> trades_buffer_{};
    std::array<char, kIoBufferBytes> depth_buffer_{};
    // One row's worth of text, reused so a recording allocates nothing per event.
    std::string row_;
    // Events written since the last flush. A long run is flushed as it goes, so one that is stopped
    // rather than closed loses at most this many events instead of the stream's whole buffer.
    std::size_t events_{0};
    RecordingSummary summary_;
    // Which shape the depth file received, decided by the first depth event it saw.
    enum class DepthShape : uint8_t
    {
        Unset = 0,
        Incremental,
        Snapshot
    };
    DepthShape depth_shape_{DepthShape::Unset};
};

// True when a field can round-trip through the replay CSV, which is comma separated and not quoted.
bool replay_field_is_writable(std::string_view value);
