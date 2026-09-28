#pragma once

#include <functional>
#include <memory>

#include "market/market_data_adapter.h"
#include "market/market_data_validator.h"
#include "utils/stage_profiler.h"

class L2ReplayFeed
{
   public:
    using EventCallback = std::function<void(const MarketEvent&)>;

    L2ReplayFeed(std::unique_ptr<IMarketEventReader> trades,
                 std::unique_ptr<IMarketEventReader> depth,
                 EventCallback callback, int ms_delay = 0,
                 MarketDataValidationConfig validation_config = {});

    void run();
    const MarketDataValidationSummary& validation_summary() const
    {
        return validator_.summary();
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

    std::unique_ptr<IMarketEventReader> trades_;
    std::unique_ptr<IMarketEventReader> depth_;
    EventCallback callback_;
    int ms_delay_;
    MarketDataValidator validator_;
    StageProfiler* profiler_{nullptr};
};
