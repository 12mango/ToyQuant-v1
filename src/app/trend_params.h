#pragma once

#include <cstdint>

// The non-market-making signal's knobs, in one place because both the command line and the factory need the
// type. `set` is what keeps the strategy's own defaults in `TrendL2Config`: a run that does not name
// --trend-params leaves every number here unread rather than copying today's defaults into a second place
// that would drift from them.
struct TrendParams
{
    uint64_t fast_views{10};
    uint64_t slow_views{60};
    double entry_ticks{2.0};
    double exit_ticks{0.5};
    double stop_ticks{6.0};
    uint64_t exit_cooldown_views{60};
    uint64_t max_holding_views{2000};
    bool set{false};
};
