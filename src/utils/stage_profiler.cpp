#include "utils/stage_profiler.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>

namespace
{
constexpr const char* kStageNames[] = {"read+parse", "validate",    "book-apply", "market-view",
                                       "top-booking", "engine",     "queue-acct", "strategy"};

static_assert(sizeof(kStageNames) / sizeof(kStageNames[0]) ==
                  static_cast<std::size_t>(Stage::Count),
              "stage name table must have one entry per Stage");

double mean_of(const std::vector<std::uint64_t>& values)
{
    if (values.empty()) return 0.0;
    std::uint64_t total = 0;
    for (const std::uint64_t value : values) total += value;
    return static_cast<double>(total) / static_cast<double>(values.size());
}
}  // namespace

const char* stage_name(Stage stage)
{
    const auto index = static_cast<std::size_t>(stage);
    return index < static_cast<std::size_t>(Stage::Count) ? kStageNames[index] : "unknown";
}

StageProfiler::StageProfiler(std::uint64_t sample_interval, std::size_t max_samples_per_stage)
    : sample_interval_(std::max<std::uint64_t>(sample_interval, 1)),
      max_samples_per_stage_(max_samples_per_stage)
{
    // Calibrate the instrument before using it. Two back to back clock reads measure the
    // overhead that every region would otherwise be charged: the cost of the second read
    // lands partly inside the interval being timed. The minimum of several probes is the
    // least disturbed estimate.
    std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
    for (int probe = 0; probe < 16; ++probe)
    {
        const std::uint64_t opened = now_ns();
        const std::uint64_t closed = now_ns();
        best = std::min(best, closed - opened);
    }
    clock_overhead_ns_ = best == std::numeric_limits<std::uint64_t>::max() ? 0 : best;
}

std::uint64_t StageProfiler::now_ns()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool StageProfiler::start_event()
{
    if (sampled_) commit();
    ++events_;
    sampled_ = (events_ % sample_interval_) == 0;
    if (sampled_) ++samples_;
    return sampled_;
}

void StageProfiler::commit()
{
    for (std::size_t index = 0; index < kStageCount; ++index)
    {
        if (!touched_[index]) continue;
        touched_[index] = false;
        StageSamples& samples = measured_[index];
        if (samples.values.size() < max_samples_per_stage_)
            samples.values.push_back(accumulated_[index]);
        else
            ++samples.truncated;
        accumulated_[index] = 0;
    }
}

double StageProfiler::stage_mean_ns(Stage stage) const
{
    return mean_of(measured_[index_of(stage)].values);
}

std::string StageProfiler::report()
{
    if (sampled_) commit();

    std::ostringstream out;
    out << "[PROFILE] L2 replay stage timings in nanoseconds per event\n";
    out << "[PROFILE] sampled every " << sample_interval_ << " events: " << samples_
        << " samples out of " << events_ << " events\n";
    out << "[PROFILE] clock overhead " << clock_overhead_ns_ << " ns per region, already subtracted\n";
    if (samples_ == 0)
    {
        out << "[PROFILE] no samples: the run was shorter than one sample interval\n";
        return out.str();
    }

    struct Row
    {
        double mean{0.0};
        std::uint64_t p50{0};
        std::uint64_t p90{0};
        std::uint64_t p99{0};
        std::uint64_t p999{0};
        std::uint64_t max{0};
    };

    std::array<Row, kStageCount> rows{};
    std::array<std::size_t, kStageCount> counts{};
    double mean_total = 0.0;
    std::uint64_t p50_total = 0;
    for (std::size_t index = 0; index < kStageCount; ++index)
    {
        const auto& raw = measured_[index].values;
        std::vector<std::uint64_t> sorted = raw;
        std::sort(sorted.begin(), sorted.end());
        const auto at = [&sorted](double fraction) -> std::uint64_t
        {
            if (sorted.empty()) return 0;
            const auto rank =
                static_cast<std::size_t>(fraction * static_cast<double>(sorted.size()));
            return sorted[std::min(rank, sorted.size() - 1)];
        };

        counts[index] = raw.size();
        rows[index] = Row{mean_of(raw), at(0.50), at(0.90), at(0.99), at(0.999), at(1.0)};
        mean_total += rows[index].mean;
        p50_total += rows[index].p50;
    }

    // Shares use the p50 column: it is the stable one, and its sum is the quantity that
    // reconciles with the per-event wall clock time.
    const auto share_of = [p50_total](std::uint64_t p50)
    {
        return p50_total > 0 ? 100.0 * static_cast<double>(p50) / static_cast<double>(p50_total)
                             : 0.0;
    };

    out << "[PROFILE] " << std::left << std::setw(14) << "stage" << std::right << std::setw(9)
        << "samples" << std::setw(9) << "share" << std::setw(11) << "mean" << std::setw(11)
        << "p50" << std::setw(11) << "p90" << std::setw(11) << "p99" << std::setw(11) << "p999"
        << std::setw(11) << "max" << "\n";

    for (std::size_t index = 0; index < kStageCount; ++index)
    {
        out << "[PROFILE] " << std::left << std::setw(14)
            << stage_name(static_cast<Stage>(index)) << std::right << std::setw(9) << counts[index]
            << std::fixed << std::setprecision(1) << std::setw(8) << share_of(rows[index].p50)
            << "%" << std::setw(11) << rows[index].mean << std::setw(11) << rows[index].p50
            << std::setw(11) << rows[index].p90 << std::setw(11) << rows[index].p99
            << std::setw(11) << rows[index].p999 << std::setw(11) << rows[index].max << "\n";
        if (measured_[index].truncated > 0)
            out << "[PROFILE]   note: " << measured_[index].truncated
                << " further samples dropped for this stage (cap " << max_samples_per_stage_
                << ")\n";
    }

    out << "[PROFILE] " << std::left << std::setw(14) << "column sum" << std::right << std::setw(9)
        << "" << std::fixed << std::setprecision(1) << std::setw(8) << 100.0 << "%" << std::setw(11)
        << mean_total << std::setw(11) << p50_total << "\n";
    out << "[PROFILE] note: the column sum row is the sum of the rows above, not a percentile.\n";
    out << "[PROFILE] note: share uses the p50 column. The mean is unstable when a stage has rare\n";
    out << "[PROFILE] note: multi-microsecond stalls, and p999/max are exactly those stalls.\n";
    out << "[PROFILE] note: the p50 column sum is the figure to compare against the per-event\n";
    out << "[PROFILE] note: wall clock time, which is the run duration divided by the event count.\n";
    return out.str();
}
