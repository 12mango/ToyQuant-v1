// The checks in this test are the test: keep them enabled even when the build defines
// NDEBUG, which is the case for the RelWithDebInfo profiling preset.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "utils/stage_profiler.h"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <thread>

int main()
{
    // Only every interval-th event is timed, and the count is exact.
    {
        StageProfiler profiler(4);
        std::size_t sampled = 0;
        for (int event = 1; event <= 10; ++event)
        {
            if (!profiler.start_event()) continue;
            ++sampled;
            profiler.begin(Stage::ReadParse);
            profiler.end(Stage::ReadParse);
        }
        assert(profiler.events() == 10);
        assert(sampled == 2);  // events 4 and 8
        assert(profiler.samples() == 2);
        assert(profiler.stage_samples(Stage::ReadParse) == 2);
    }

    // A stage bracketed twice in one event contributes one sample, not two, and stages
    // that never ran contribute nothing.
    {
        StageProfiler profiler(1);
        for (int event = 0; event < 5; ++event)
        {
            assert(profiler.start_event());
            profiler.begin(Stage::TopBookKeeping);
            profiler.end(Stage::TopBookKeeping);
            profiler.begin(Stage::Engine);
            profiler.end(Stage::Engine);
            profiler.begin(Stage::TopBookKeeping);
            profiler.end(Stage::TopBookKeeping);
        }
        profiler.report();
        assert(profiler.samples() == 5);
        assert(profiler.stage_samples(Stage::TopBookKeeping) == 5);
        assert(profiler.stage_samples(Stage::Engine) == 5);
        assert(profiler.stage_samples(Stage::MarketView) == 0);
        assert(profiler.stage_samples(Stage::Strategy) == 0);
    }

    // Two segments in one event are summed, not overwritten. Each segment sleeps, so the
    // recorded value has to cover both.
    {
        StageProfiler profiler(1);
        assert(profiler.start_event());
        for (int segment = 0; segment < 2; ++segment)
        {
            profiler.begin(Stage::Engine);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            profiler.end(Stage::Engine);
        }
        // Still uncommitted: report() flushes the in-flight event.
        assert(profiler.stage_samples(Stage::Engine) == 0);
        profiler.report();
        assert(profiler.stage_samples(Stage::Engine) == 1);
        assert(profiler.stage_mean_ns(Stage::Engine) >= 3.0e6);
    }

    // The report names every stage and flags a run that was too short to sample.
    {
        StageProfiler profiler(1000);
        profiler.start_event();
        const std::string report = profiler.report();
        assert(profiler.samples() == 0);
        assert(report.find("no samples") != std::string::npos);
    }
    {
        StageProfiler profiler(1);
        assert(profiler.start_event());
        profiler.begin(Stage::Validate);
        profiler.end(Stage::Validate);
        const std::string report = profiler.report();
        assert(report.find("validate") != std::string::npos);
        assert(report.find("column sum") != std::string::npos);
        assert(report.find("sampled every 1 events") != std::string::npos);
    }

    // The instrument calibrates its own clock overhead, and the calibration has to be
    // positive on any realistic host.
    {
        StageProfiler profiler(1);
        assert(profiler.clock_overhead_ns() > 0);
    }

    for (std::size_t index = 0; index < static_cast<std::size_t>(Stage::Count); ++index)
        assert(std::string(stage_name(static_cast<Stage>(index))) != "unknown");

    std::cout << "stage_profiler_test passed\n";
    return 0;
}
