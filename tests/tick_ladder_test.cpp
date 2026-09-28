// The checks in this test are the test: keep them enabled even when the build defines
// NDEBUG, which is the case for the RelWithDebInfo profiling preset.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include "orderbook/tick_ladder.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <random>

namespace
{
// Compares a sample of ladder levels against the reference map: the first few, the last, and one
// past the end.
template <typename Levels>
void check_levels(const Levels& reference, const TickLadder& ladder, std::size_t size)
{
    for (const std::size_t index : {std::size_t{0}, std::size_t{1}, std::size_t{2}})
    {
        if (index >= size) continue;
        PriceTick out_tick = 0;
        std::uint64_t out_quantity = 0;
        assert(ladder.nth(index, out_tick, out_quantity));
        auto it = reference.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(index));
        assert(out_tick == it->first);
        assert(out_quantity == it->second);
    }

    if (size > 0)
    {
        PriceTick out_tick = 0;
        std::uint64_t out_quantity = 0;
        assert(ladder.nth(size - 1, out_tick, out_quantity));
        assert(out_tick == reference.rbegin()->first);
    }

    PriceTick out_tick = 0;
    std::uint64_t out_quantity = 0;
    assert(!ladder.nth(size, out_tick, out_quantity));
}

// The ladder replaced a std::map, so a map is the reference it has to match exactly.
void differential_against_map(bool descending, std::uint64_t seed, std::size_t steps)
{
    std::mt19937_64 rng(seed);
    TickLadder ladder(descending);
    std::map<PriceTick, std::uint64_t, std::greater<PriceTick>> descending_reference;
    std::map<PriceTick, std::uint64_t> ascending_reference;

    for (std::size_t step = 0; step < steps; ++step)
    {
        const PriceTick tick = static_cast<PriceTick>(500 + rng() % 3000);
        const std::uint64_t quantity = (rng() % 4 == 0) ? 0 : (rng() % 1000 + 1);
        ladder.set(tick, quantity);

        PriceTick expected_best = TickLadder::kNoBest;
        std::size_t expected_size = 0;
        if (descending)
        {
            if (quantity == 0) descending_reference.erase(tick);
            else descending_reference[tick] = quantity;
            expected_size = descending_reference.size();
            if (!descending_reference.empty())
                expected_best = descending_reference.begin()->first;
        }
        else
        {
            if (quantity == 0) ascending_reference.erase(tick);
            else ascending_reference[tick] = quantity;
            expected_size = ascending_reference.size();
            if (!ascending_reference.empty()) expected_best = ascending_reference.begin()->first;
        }

        assert(ladder.size() == expected_size);
        assert(ladder.best() == expected_best);
        assert(ladder.empty() == (expected_size == 0));

        // Sampling keeps this under a second; a stale hint or a lost level still shows up.
        if (step % 16 != 0) continue;
        if (descending)
            check_levels(descending_reference, ladder, expected_size);
        else
            check_levels(ascending_reference, ladder, expected_size);
    }
}
}  // namespace

int main()
{
    // Ticks far outside the initial range must grow the array without losing or reordering
    // anything. This is the property that lets the price window be learned instead of configured.
    {
        TickLadder ladder(true);
        ladder.set(12842, 10);
        assert(ladder.size() == 1);
        assert(ladder.best() == 12842);

        ladder.set(2000, 20);   // far below the window the first set() created
        ladder.set(42000, 30);  // far above it
        ladder.set(12843, 40);
        PriceTick tick = 0;
        std::uint64_t quantity = 0;
        assert(ladder.nth(0, tick, quantity) && tick == 42000 && quantity == 30);
        assert(ladder.nth(1, tick, quantity) && tick == 12843 && quantity == 40);
        assert(ladder.nth(2, tick, quantity) && tick == 12842 && quantity == 10);
        assert(ladder.nth(3, tick, quantity) && tick == 2000 && quantity == 20);
        assert(!ladder.nth(4, tick, quantity));
        assert(ladder.size() == 4);

        // Setting the same value again is a no-op, and a zero removes the level.
        ladder.set(12843, 40);
        assert(ladder.size() == 4);
        ladder.set(12843, 0);
        assert(ladder.size() == 3);
        assert(ladder.nth(1, tick, quantity) && tick == 12842);
    }

    // Removing the best level has to move the hint to the next populated one, including across
    // empty slots, and back to kNoBest when the side empties.
    {
        TickLadder ladder(false);
        ladder.set(100, 1);
        ladder.set(104, 2);
        ladder.set(110, 3);
        assert(ladder.best() == 100);
        ladder.set(100, 0);
        assert(ladder.best() == 104);
        ladder.set(104, 0);
        assert(ladder.best() == 110);
        ladder.set(110, 0);
        assert(ladder.best() == TickLadder::kNoBest);
        assert(ladder.empty());

        ladder.set(200, 7);
        assert(ladder.best() == 200);
        ladder.clear();
        assert(ladder.empty());
        assert(ladder.best() == TickLadder::kNoBest);
        assert(ladder.size() == 0);
    }

    // The same on the descending side, where the hint walks toward lower ticks.
    {
        TickLadder ladder(true);
        ladder.set(100, 1);
        ladder.set(104, 2);
        ladder.set(110, 3);
        assert(ladder.best() == 110);
        ladder.set(110, 0);
        assert(ladder.best() == 104);
        ladder.set(104, 0);
        assert(ladder.best() == 100);
        ladder.set(100, 0);
        assert(ladder.best() == TickLadder::kNoBest);
    }

    differential_against_map(true, 0x5eed1, 6000);
    differential_against_map(false, 0x5eed2, 6000);
    differential_against_map(true, 0x5eed3, 8000);

    std::cout << "tick_ladder_test passed\n";
    return 0;
}
