#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "market/market_data_adapter.h"
#include "market/market_data_validator.h"
#include "utils/stage_profiler.h"

// How a replay is delivered against the wall clock. Without pacing every event is delivered as fast as the file
// can be read, which is right for a measurement and wrong for a rehearsal: a strategy that watches its own quote
// age, cancels on a timer, or makes any decision that depends on how long it has been waiting behaves
// differently when the market arrives at its real speed. Pacing maps the file's timestamps onto the wall clock
// and reports how late the delivery ran, which is the feed-side number a live session has to watch.
struct ReplayPacing
{
    bool enabled{false};
    // 1.0 is real time; 10.0 delivers ten seconds of market in one, which is how an afternoon checks a day.
    double speed{1.0};
    // A delivery later than this is counted as behind instead of being waited for, so a feed that has already
    // fallen behind cannot fall further by sleeping. It catches up by skipping the wait.
    int64_t max_lateness_ns{50'000'000};
};

// What the paced delivery achieved. Lateness is measured against the schedule the file's own timestamps define,
// so it is the sum of everything between the schedule and the delivery: this process being busy, the reader
// being slow, and the operating system not waking the thread on time.
struct PacingSummary
{
    uint64_t events{0};
    uint64_t late_events{0};
    int64_t lateness_max_ns{0};
    int64_t lateness_mean_ns{0};
    // Bucketed by powers of two, so a percentile is reported to within a factor of two rather than to a
    // microsecond. Exact percentiles would need one sample per event, which is tens of megabytes on a window
    // this repository replays.
    int64_t lateness_p50_ns{0};
    int64_t lateness_p99_ns{0};
    int64_t market_window_ns{0};
    int64_t wall_ns{0};
    double speed{1.0};

    std::string to_log_string() const;
};

class L2ReplayFeed
{
   public:
    using EventCallback = std::function<void(const MarketEvent&)>;
    // The two seams that make pacing testable: a monotonic clock in nanoseconds, and a sleep in nanoseconds.
    // The defaults are the real ones, and a test replaces both with a clock it advances by hand, so a timing
    // behaviour can be asserted exactly instead of waited for.
    using Clock = std::function<int64_t()>;
    using Sleeper = std::function<void(int64_t)>;

    L2ReplayFeed(std::unique_ptr<IMarketEventReader> trades,
                 std::unique_ptr<IMarketEventReader> depth,
                 EventCallback callback, ReplayPacing pacing = {},
                 MarketDataValidationConfig validation_config = {});

    void run();
    const MarketDataValidationSummary& validation_summary() const
    {
        return validator_.summary();
    }

    // Only meaningful when pacing is on; zeroed otherwise.
    const PacingSummary& pacing_summary() const
    {
        return pacing_summary_;
    }

    void set_clock(Clock clock)
    {
        clock_ = std::move(clock);
    }

    void set_sleeper(Sleeper sleeper)
    {
        sleeper_ = std::move(sleeper);
    }

    // Optional per-stage timing. The profiler is owned by the caller and has to outlive
    // the feed. Passing nullptr, which is the default, keeps the replay loop unchanged.
    void set_profiler(StageProfiler* profiler)
    {
        profiler_ = profiler;
    }

   private:
    // One sampled clock read per stage boundary; a plain nullptr check when profiling is off.
    void profiler_begin(Stage stage)
    {
        if (profiler_ != nullptr) profiler_->begin(stage);
    }

    void profiler_end(Stage stage)
    {
        if (profiler_ != nullptr) profiler_->end(stage);
    }

    // Wait until the event's place in the schedule, and record how late it was.
    void pace(int64_t event_ns);
    void record_lateness(int64_t lateness_ns);

    std::unique_ptr<IMarketEventReader> trades_;
    std::unique_ptr<IMarketEventReader> depth_;
    EventCallback callback_;
    ReplayPacing pacing_;
    MarketDataValidator validator_;
    StageProfiler* profiler_{nullptr};
    Clock clock_;
    Sleeper sleeper_;
    bool started_{false};
    int64_t first_event_ns_{0};
    int64_t start_wall_ns_{0};
    PacingSummary pacing_summary_{};
    // Powers of two in microseconds: bucket i counts lateness in [2^(i-1), 2^i) microseconds, and the last
    // bucket collects everything slower than its floor.
    static constexpr std::size_t kLatenessBuckets = 24;
    std::array<uint64_t, kLatenessBuckets> lateness_buckets_{};
    uint64_t lateness_count_{0};
    int64_t lateness_sum_ns_{0};
};

