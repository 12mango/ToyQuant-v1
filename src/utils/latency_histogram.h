#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

/*
-------------------------------------------
 LatencyHistogram
-------------------------------------------

Bucketed latency samples in powers of two microseconds, with a percentile reported as the upper edge
of the bucket the sample fell in and clamped to the observed maximum.

Why buckets rather than one sample per event: the windows this repository replays deliver hundreds of
thousands of events, so one sample each would cost megabytes per histogram per stage, and the numbers
being compared differ by factors rather than by microseconds. The clamp exists because without it a
reported p99 can exceed the largest sample, which reads as a broken metric even though it is the
histogram's granularity showing through; the report therefore says microseconds and carries no
precision it does not have.

The paced replay's lateness report uses the same convention, which is why the rule lives here rather
than beside either caller: two implementations of a subtle percentile rule is one too many.
-------------------------------------------
*/

class LatencyHistogram
{
   public:
    // Powers of two microseconds, so this many buckets reach about 35 minutes. A sample beyond that
    // lands in the last bucket and is reported at the observed maximum.
    static constexpr std::size_t kBuckets = 32;

    void add(int64_t nanoseconds)
    {
        if (nanoseconds < 0) nanoseconds = 0;
        std::size_t bucket = 0;
        int64_t step = 1;
        while (bucket + 1 < kBuckets && nanoseconds >= step * 1000)
        {
            ++bucket;
            step *= 2;
        }
        ++buckets_[bucket];
        ++count_;
        sum_ns_ += nanoseconds;
        if (nanoseconds > max_ns_) max_ns_ = nanoseconds;
    }

    uint64_t count() const
    {
        return count_;
    }

    // Exact, because the sum is one 64-bit integer: nanoseconds over any window this project runs
    // stay far below its overflow point.
    int64_t mean_ns() const
    {
        return count_ == 0 ? 0 : sum_ns_ / static_cast<int64_t>(count_);
    }

    int64_t max_ns() const
    {
        return max_ns_;
    }

    // Upper edge of the bucket this fraction falls in, clamped to the largest sample. The first
    // bucket holds everything under a microsecond and reports one, so a percentile is never zero for
    // a histogram that has samples.
    int64_t percentile_ns(double fraction) const
    {
        if (count_ == 0) return 0;
        const uint64_t target =
            std::max<uint64_t>(1, static_cast<uint64_t>(fraction * static_cast<double>(count_)));
        uint64_t seen = 0;
        for (std::size_t bucket = 0; bucket < buckets_.size(); ++bucket)
        {
            seen += buckets_[bucket];
            if (seen >= target && buckets_[bucket] > 0)
                return std::min(static_cast<int64_t>(bucket == 0 ? 1 : (int64_t{1} << bucket)) * 1000,
                                max_ns_);
        }
        return max_ns_;
    }

   private:
    std::array<uint64_t, kBuckets> buckets_{};
    uint64_t count_{0};
    int64_t sum_ns_{0};
    int64_t max_ns_{0};
};
