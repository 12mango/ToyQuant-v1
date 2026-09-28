#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/*
-------------------------------------------
 StageProfiler
-------------------------------------------

A sampled stopwatch for the steps that turn one market data row into one
quote. It answers "how long is each step", which a function level
profiler such as gprof cannot answer well: gprof adds a call at the entry
of every function, so tiny helpers are charged for the measurement itself
and inlined helpers are charged to their caller.

Timing every event would distort what it measures. Reading the monotonic
clock costs roughly 20 ns, eight stages with two reads each is about
320 ns, and one event currently costs about 600 ns. Only every
sample_interval-th event is timed instead, which keeps the overhead near
one branch per stage and still leaves tens of thousands of samples.

Because samples are rare they are stored as raw nanosecond values rather
than bucketed, so the report contains exact percentiles instead of
histogram edges.

Contract for callers:
  * Call start_event() exactly once per event, before any begin().
  * begin()/end() bracket one region. A stage may be bracketed several
    times within the same event; the times are summed so that each stage
    still contributes exactly one sample per event.
  * commit() happens on the next start_event() and in report(), so the
    final event is included as long as report() is called.
-------------------------------------------
*/

enum class Stage : std::uint8_t
{
    ReadParse = 0,     // reader next(): block refill, memchr, field split, number conversion
    Validate,          // MarketDataValidator::validate
    BookApply,         // L2OrderBook::apply_snapshot / apply_incremental_batch
    MarketView,        // L2OrderBook::market_view
    TopBookKeeping,    // Pipeline: latest_quotes_, last_mid_, quote_cycle_
    Engine,            // MatchingEngine::process_l2_top
    QueueAccounting,   // Pipeline: queue_ahead_consumed deltas and on_queue_activity
    Strategy,          // strategy decision plus order submission
    Count
};

const char* stage_name(Stage stage);

class StageProfiler
{
   public:
    static constexpr std::uint64_t kDefaultSampleInterval = 64;
    static constexpr std::size_t kDefaultMaxSamplesPerStage = 2000000;

    explicit StageProfiler(
        std::uint64_t sample_interval = kDefaultSampleInterval,
        std::size_t max_samples_per_stage = kDefaultMaxSamplesPerStage);

    // Advances the event counter, commits the previous event, and reports whether the
    // current event is timed.
    bool start_event();

    // True when the current event is timed, i.e. begin()/end() will record.
    bool sampled() const
    {
        return sampled_;
    }

    // Both are cheap no-ops for events that are not sampled.
    void begin(Stage stage)
    {
        if (!sampled_) return;
        started_[index_of(stage)] = now_ns();
    }

    void end(Stage stage)
    {
        if (!sampled_) return;
        const std::size_t index = index_of(stage);
        const std::uint64_t elapsed = now_ns() - started_[index];
        // Two clock reads bracket every region, and the second one is partly inside the
        // measured interval. Subtracting the calibrated cost keeps the reported values
        // from being systematically high.
        accumulated_[index] += elapsed > clock_overhead_ns_ ? elapsed - clock_overhead_ns_ : 0;
        touched_[index] = true;
    }

    std::uint64_t events() const
    {
        return events_;
    }

    std::uint64_t samples() const
    {
        return samples_;
    }

    // Number of samples collected for one stage. Stages that do not run for every
    // event, such as MarketView, end up with fewer samples than `samples()`.
    std::size_t stage_samples(Stage stage) const
    {
        return measured_[index_of(stage)].values.size();
    }

    // Mean of the collected samples for one stage, in nanoseconds; zero when there are
    // none. commit() happens inside report(), so call this after report() to include the
    // final event.
    double stage_mean_ns(Stage stage) const;

    // Cost of the two clock reads that bracket a region, measured once at construction.
    // It is subtracted from every sample.
    std::uint64_t clock_overhead_ns() const
    {
        return clock_overhead_ns_;
    }

    // Multi-line table with per-stage mean, p50, p90, p99, p999 and max in nanoseconds.
    // Commits the in-flight event first, so call it after the last event.
    std::string report();

   private:
    static constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::Count);

    struct StageSamples
    {
        std::vector<std::uint64_t> values;
        std::uint64_t truncated{0};
    };

    static std::size_t index_of(Stage stage)
    {
        return static_cast<std::size_t>(stage);
    }

    static std::uint64_t now_ns();
    void commit();

    std::uint64_t sample_interval_;
    std::size_t max_samples_per_stage_;
    std::uint64_t events_{0};
    std::uint64_t samples_{0};
    std::uint64_t clock_overhead_ns_{0};
    bool sampled_{false};
    std::array<std::uint64_t, kStageCount> started_{};
    std::array<std::uint64_t, kStageCount> accumulated_{};
    std::array<bool, kStageCount> touched_{};
    std::array<StageSamples, kStageCount> measured_{};
};
