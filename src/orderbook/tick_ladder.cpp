#include "orderbook/tick_ladder.h"

#include <algorithm>

TickLadder::TickLadder(bool descending) : descending_(descending)
{
}

void TickLadder::grow_to_cover(PriceTick tick)
{
    // Sized around the first price seen, then grown with a margin so that a drifting market does
    // not reallocate on consecutive updates. Growth is rare: the Deribit incremental feed settles
    // into a stable range within the first snapshot.
    constexpr std::size_t kInitialSlots = 2048;
    constexpr std::size_t kGrowthMargin = 1024;

    PriceTick new_base = base_;
    std::size_t new_slots = quantity_.size();
    if (quantity_.empty())
    {
        const PriceTick half = static_cast<PriceTick>(kInitialSlots / 2);
        new_base = tick - half;
        new_slots = kInitialSlots;
    }
    else
    {
        const PriceTick old_top = base_ + static_cast<PriceTick>(quantity_.size());
        new_base = std::min(base_, tick) - static_cast<PriceTick>(kGrowthMargin);
        const PriceTick new_top =
            std::max(old_top, tick + 1) + static_cast<PriceTick>(kGrowthMargin);
        new_slots = static_cast<std::size_t>(new_top - new_base);
    }
    if (new_base < 0)
    {
        // Ticks are derived from positive prices, but keep the base non-negative so that slot
        // arithmetic stays in unsigned range.
        new_slots += static_cast<std::size_t>(-new_base);
        new_base = 0;
    }

    std::vector<uint64_t> grown(new_slots, uint64_t{0});
    if (!quantity_.empty())
    {
        const std::size_t offset = static_cast<std::size_t>(base_ - new_base);
        for (std::size_t slot = 0; slot < quantity_.size(); ++slot)
            grown[slot + offset] = quantity_[slot];
    }
    quantity_.swap(grown);
    base_ = new_base;
}

void TickLadder::advance_best_hint()
{
    if (count_ == 0)
    {
        best_ = kNoBest;
        return;
    }

    std::size_t slot = slot_of(best_);
    if (descending_)
    {
        // The emptied slot was the best, so every populated level is at a lower tick.
        while (slot > 0)
        {
            --slot;
            if (quantity_[slot] != 0)
            {
                best_ = base_ + static_cast<PriceTick>(slot);
                return;
            }
        }
    }
    else
    {
        const std::size_t slots = quantity_.size();
        while (++slot < slots)
        {
            if (quantity_[slot] != 0)
            {
                best_ = base_ + static_cast<PriceTick>(slot);
                return;
            }
        }
    }
    best_ = kNoBest;
}
