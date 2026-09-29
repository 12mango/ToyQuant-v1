#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "common/types.h"

/*
-------------------------------------------
 TickLadder
-------------------------------------------

 One side of the L2 book, stored as a flat array indexed by price tick.

 A std::map and a sorted vector both walk a level in a number of dependent steps: a red-black tree
 descends about ten pointer levels, and a sorted vector moves several kilobytes of elements per
 insertion once the book holds a thousand levels. Deribit BTC-PERPETUAL keeps more than a thousand
 live levels per side across a span of about twenty thousand ticks, so both structures pay for that
 shape. Indexing by tick turns a level change into one computed address.

 The price range is learned rather than configured. The array is sized around the first price it
 sees and grows when an update arrives outside the current range, so no fixed window is assumed and
 no update is dropped.

 Iteration walks from a remembered best slot. Removing that slot advances the hint to the next
 populated one, which real books keep close because they are dense near the touch. The hint is only
 written on the update path, so reads stay pure.

 Thread safety: L2OrderBook holds its own mutex around every call, so the hint is never shared
 between threads.
-------------------------------------------
*/
class TickLadder
{
   public:
    static constexpr PriceTick kNoBest = -1;

    // `descending` selects bids, where the best level is the highest tick.
    explicit TickLadder(bool descending);

    // Sets the quantity at `tick`. Zero removes the level. The range grows to cover the tick.
    void set(PriceTick tick, uint64_t quantity)
    {
        if (!in_range(tick)) grow_to_cover(tick);
        const std::size_t slot = slot_of(tick);
        const uint64_t previous = quantity_[slot];
        // A repeated value is common in an incremental feed and needs no write at all.
        if (previous == quantity) return;
        quantity_[slot] = quantity;
        if (quantity == 0)
        {
            --count_;
            if (best_ == tick) advance_best_hint();
        }
        else if (previous == 0)
        {
            ++count_;
            if (best_ == kNoBest) best_ = tick;
            else if (descending_ ? (tick > best_) : (tick < best_)) best_ = tick;
        }
    }

    void clear()
    {
        // A zero count means every slot is already zero, so only a populated ladder needs filling.
        if (count_ > 0) std::fill(quantity_.begin(), quantity_.end(), uint64_t{0});
        count_ = 0;
        best_ = kNoBest;
    }

    bool empty() const
    {
        return count_ == 0;
    }

    std::size_t size() const
    {
        return count_;
    }

    // Best populated tick, or kNoBest when this side is empty.
    PriceTick best() const
    {
        return best_;
    }

    // The touch and the summed depth of the best `levels` populated slots, produced by one walk.
    // Asking nth() for each level separately restarts the walk from the best slot every time, so a
    // five level view used to cross the near-touch slots about five times over.
    struct TopLevels
    {
        PriceTick tick{kNoBest};
        uint64_t quantity{0};
        uint64_t depth{0};
    };

    TopLevels top_levels(std::size_t levels) const
    {
        TopLevels result;
        if (levels == 0 || best_ == kNoBest) return result;

        std::size_t found = 0;
        const auto count_slot = [&](std::size_t slot)
        {
            const uint64_t quantity = quantity_[slot];
            if (quantity == 0) return false;
            if (found == 0)
            {
                result.tick = base_ + static_cast<PriceTick>(slot);
                result.quantity = quantity;
            }
            result.depth += quantity;
            return ++found == levels;
        };

        if (descending_)
        {
            std::size_t slot = slot_of(best_);
            for (;;)
            {
                if (count_slot(slot)) break;
                if (slot == 0) break;
                --slot;
            }
        }
        else
        {
            const std::size_t slots = quantity_.size();
            for (std::size_t slot = slot_of(best_);;)
            {
                if (count_slot(slot)) break;
                if (++slot >= slots) break;
            }
        }
        return result;
    }

    // The index-th best level. Returns false when this side holds fewer levels.
    bool nth(std::size_t index, PriceTick& tick, uint64_t& quantity) const
    {
        if (index >= count_ || best_ == kNoBest) return false;
        std::size_t slot = slot_of(best_);
        std::size_t found = 0;
        if (descending_)
        {
            for (;;)
            {
                if (quantity_[slot] != 0)
                {
                    if (found == index)
                    {
                        tick = base_ + static_cast<PriceTick>(slot);
                        quantity = quantity_[slot];
                        return true;
                    }
                    ++found;
                }
                if (slot == 0) break;
                --slot;
            }
        }
        else
        {
            const std::size_t slots = quantity_.size();
            for (;;)
            {
                if (quantity_[slot] != 0)
                {
                    if (found == index)
                    {
                        tick = base_ + static_cast<PriceTick>(slot);
                        quantity = quantity_[slot];
                        return true;
                    }
                    ++found;
                }
                if (++slot >= slots) break;
            }
        }
        return false;
    }

   private:
    // Grows the range so that `tick` is representable, preserving the contents.
    void grow_to_cover(PriceTick tick);

    // Moves the hint to the next populated slot after the current best was emptied.
    void advance_best_hint();

    bool in_range(PriceTick tick) const
    {
        return tick >= base_ && tick - base_ < static_cast<PriceTick>(quantity_.size());
    }

    std::size_t slot_of(PriceTick tick) const
    {
        return static_cast<std::size_t>(tick - base_);
    }

    bool descending_;
    PriceTick base_{0};
    std::size_t count_{0};
    std::vector<uint64_t> quantity_;
    PriceTick best_{kNoBest};
};
