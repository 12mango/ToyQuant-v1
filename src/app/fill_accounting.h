#pragma once

#include <cstdint>

#include "app/run_summary.h"
#include "common/types.h"
#include "exchange/execution_report.h"

class RunAccounting;

// The inputs the fill accounting needs that are not part of its own state. The values live in Pipeline
// because they also configure the engine and the report; passing them per fill keeps this component from
// owning a second copy that could drift from the run's settings.
struct FillContext
{
    double last_mid{0.0};
    double tick_size{PRICE_TICK_SIZE};
    double unit_notional_usd{0.0};
    double multiplier{1.0};
    uint64_t quantity_scale{1};
    double starting_cash_usd{0.0};
    uint64_t quote_cycle{0};
};

// The state changed by our own fills: position, the quantity reserved by the position gate, maker notional,
// exposure and the trade counters. It used to be eight fields inside Pipeline, updated by a hundred-line
// report callback. Keeping it here lets a non-MM path replace the fill accounting without copying the
// report fan-out, and gives the summary one place to read.
class FillAccounting
{
   public:
    FillAccounting(ExecutionQualityMetrics& quality, RunAccounting& accounting);

    // Applies one of our Trade reports. The caller has already checked owner and exec type.
    void apply(const ExecutionReport& report, const FillContext& context);

    // The position gate reserves what it lets through; a terminal report or a refusal releases it.
    void reserve(Side side, uint64_t quantity);
    void release(Side side, uint64_t quantity, bool sell_on_unknown);

    int64_t position() const
    {
        return position_;
    }

    int64_t pending_buy_quantity() const
    {
        return pending_buy_quantity_;
    }

    int64_t pending_sell_quantity() const
    {
        return pending_sell_quantity_;
    }

    double maker_notional_usd() const
    {
        return maker_notional_usd_;
    }

    uint64_t trade_reports() const
    {
        return trade_reports_;
    }

    uint64_t trade_report_quantity() const
    {
        return trade_report_quantity_;
    }

    double max_abs_exposure_usd() const
    {
        return max_abs_exposure_usd_;
    }

    uint64_t exposure_over_collateral_fills() const
    {
        return exposure_over_collateral_fills_;
    }

   private:
    ExecutionQualityMetrics& quality_;
    RunAccounting& accounting_;
    int64_t position_{0};
    int64_t pending_buy_quantity_{0};
    int64_t pending_sell_quantity_{0};
    double maker_notional_usd_{0.0};
    uint64_t trade_reports_{0};
    uint64_t trade_report_quantity_{0};
    double max_abs_exposure_usd_{0.0};
    uint64_t exposure_over_collateral_fills_{0};
};
