#pragma once
// The four documented readings of what happens to the queue ahead of a resting order when a level's
// displayed size falls. This arithmetic was a lambda inside `MatchingEngine::process_bbo_for`, which is why
// the difference between the models was visible only to someone reading that method: `QueueModel` is named
// on the command line, described in `docs/ARCHITECTURE.md`, and here is what the names do.
//
// Measured on this feed by tools/calibrate_queue_model.py: 99.96% of the displayed size decrease at a level
// is cancel or amendment rather than trade, and the book churns about two thousand four hundred times the
// volume that actually trades. The queue ahead of a resting order is therefore removed mostly by
// cancellations, and the only assumption about where those cancels sit that the data supports is that they
// are uniform, which shrinks the queue in proportion to the displayed size:
//
//     queue_ahead -= queue_ahead * reduction / previous_quantity
//
// A level that empties takes the whole queue with it, which is what that expression gives at
// reduction == previous_quantity. Optimistic treats every unit of the decrease as a cancel ahead, and
// Conservative removes nothing at all, which the engine answers before it reaches here and this class
// answers as well, so the two agree about the model whichever one is asked.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

#include "common/types.h"

class QueueAheadModel
{
   public:
    QueueAheadModel(QueueModel model, uint64_t lump_chunks, uint64_t seed)
        : model_(model), lump_chunks_(lump_chunks == 0 ? 1 : lump_chunks), rng_(seed)
    {
    }

    // True when this model takes nothing out of the queue ahead, which the engine uses to skip the whole
    // update rather than run it and multiply the result by zero. It is the same rule as the first line of
    // `advance_quantity`: the conservative reading is a lower bound, and the bound is that the queue never
    // shrinks.
    bool removes_nothing() const { return model_ == QueueModel::Conservative; }

    // How much of a level's displayed decrease is read as leaving from ahead of us. Inline because this is
    // the hot path -- up to twice per best bid and offer update, over a million of them in a session -- and
    // a call into another translation unit is the one cost the queue model does not have to pay.
    uint64_t advance_quantity(uint64_t reduction, uint64_t previous_quantity, uint64_t queue_ahead)
    {
        if (model_ == QueueModel::Conservative) return 0;
        if (model_ == QueueModel::Optimistic) return reduction;
        if (previous_quantity == 0) return queue_ahead;
        if (model_ == QueueModel::Lumpy)
        {
            // A level's displayed size falls in whole orders: the measured median decrease removes 74%
            // of a median level, and 99.96% of the fall is cancel or amendment rather than trade. So a
            // fall of r is read as k orders leaving, each of them ahead of us with probability
            // q = queue_ahead / previous_quantity, which gives
            //
            //     removed from ahead = (r / k) * Binomial(k, q)
            //     E[...]            = r * q                    same as pro-rata, for every k
            //     Var[...]          = r^2 * q * (1 - q) / k    falls as the fall is split more finely
            //
            // k = 1 is the lumpiest reading and matches that median; k -> infinity is the deterministic
            // pro-rata rule. The two models therefore differ in variance and not in mean, and this is
            // the one that can reproduce a single large cancellation putting an order at the front.
            const double share = std::min(1.0, static_cast<double>(queue_ahead) /
                                                   static_cast<double>(previous_quantity));
            std::binomial_distribution<uint64_t> from_ahead(lump_chunks_, share);
            const uint64_t ahead_orders = from_ahead(rng_);
            return static_cast<uint64_t>(std::llround(static_cast<double>(reduction) *
                                                      static_cast<double>(ahead_orders) /
                                                      static_cast<double>(lump_chunks_)));
        }
        return static_cast<uint64_t>((static_cast<double>(queue_ahead) *
                                      static_cast<double>(reduction)) /
                                     static_cast<double>(previous_quantity));
    }

   private:
    QueueModel model_;
    // Chunks per level decrease for QueueModel::Lumpy, and the generator that draws them. The seed is
    // fixed so a lumpy run is reproducible: one seed is a replication, not a random experiment, and the
    // spread across seeds is the quantity worth reporting.
    uint64_t lump_chunks_;
    std::mt19937_64 rng_;
};
