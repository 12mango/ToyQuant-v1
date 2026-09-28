#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <variant>
#include <vector>

#include "common/types.h"

struct MarketTrade
{
    uint64_t ts{};
    std::string symbol;
    double price{};
    uint64_t quantity{};
    Side aggressor_side{Side::Unknown};
    uint64_t sequence{};
    std::string exchange;
};

struct BboQuote
{
    uint64_t ts{};
    std::string symbol;
    double bid_price{};
    uint64_t bid_quantity{};
    double ask_price{};
    uint64_t ask_quantity{};
    uint64_t sequence{};
    std::string exchange;
};

struct DepthLevel
{
    double price{};
    uint64_t quantity{};
};

struct MarketDepthSnapshot
{
    uint64_t ts{};
    std::string symbol;
    uint64_t sequence{};
    std::vector<DepthLevel> bids;
    std::vector<DepthLevel> asks;
    std::string exchange;
};

struct IncrementalBookUpdate
{
    uint64_t exchange_ts{};
    uint64_t local_ts{};
    bool is_snapshot{false};
    Side side{Side::Unknown};
    double price{};
    uint64_t amount{};
};

/*
-------------------------------------------
 IncrementalUpdates
-------------------------------------------

 Storage for the updates of one incremental batch.

 A real incremental message carries one or two price level changes, so the first few updates are
 kept inside the event object and the batch needs no heap allocation at all. A batch that grows
 past the inline capacity moves its contents to a vector and stays there. On the Deribit
 incremental replay this removes one allocation and one free per batch, which measured at 161 ns
 per batch, about a quarter of the reader stage.

 begin() and end() always expose a single contiguous range, so range-for, std::any_of and
 operator[] behave exactly as they did when the storage was a plain vector.
-------------------------------------------
*/
class IncrementalUpdates
{
   public:
    static constexpr std::size_t kInlineCapacity = 4;

    using value_type = IncrementalBookUpdate;
    using iterator = IncrementalBookUpdate*;
    using const_iterator = const IncrementalBookUpdate*;

    IncrementalUpdates() = default;

    // Keeps brace initialisation working for callers that build a batch literal.
    IncrementalUpdates(std::initializer_list<IncrementalBookUpdate> updates)
    {
        for (const auto& update : updates) push_back(update);
    }

    std::size_t size() const
    {
        return overflow_.empty() ? count_ : overflow_.size();
    }

    bool empty() const
    {
        return size() == 0;
    }

    const_iterator begin() const
    {
        return data();
    }

    const_iterator end() const
    {
        return data() + size();
    }

    iterator begin()
    {
        return data();
    }

    iterator end()
    {
        return data() + size();
    }

    const IncrementalBookUpdate& operator[](std::size_t index) const
    {
        return data()[index];
    }

    IncrementalBookUpdate& operator[](std::size_t index)
    {
        return data()[index];
    }

    // A reservation within the inline capacity costs nothing.
    void reserve(std::size_t count)
    {
        if (count > kInlineCapacity) overflow_.reserve(count);
    }

    void clear()
    {
        count_ = 0;
        overflow_.clear();
    }

    void push_back(const IncrementalBookUpdate& update)
    {
        if (overflow_.empty())
        {
            if (count_ < kInlineCapacity)
            {
                inline_[count_++] = update;
                return;
            }
            spill_to_overflow();
        }
        overflow_.push_back(update);
    }

   private:
    IncrementalBookUpdate* data()
    {
        return overflow_.empty() ? inline_.data() : overflow_.data();
    }

    const IncrementalBookUpdate* data() const
    {
        return overflow_.empty() ? inline_.data() : overflow_.data();
    }

    void spill_to_overflow()
    {
        overflow_.reserve(kInlineCapacity * 2);
        overflow_.assign(inline_.begin(),
                         inline_.begin() + static_cast<std::ptrdiff_t>(count_));
        count_ = 0;
    }

    std::array<IncrementalBookUpdate, kInlineCapacity> inline_{};
    std::vector<IncrementalBookUpdate> overflow_;
    std::size_t count_{0};
};

struct IncrementalBookBatch
{
    uint64_t ts{};
    uint64_t exchange_ts{};
    uint64_t local_ts{};
    std::string symbol;
    IncrementalUpdates updates;
    std::string exchange;
    bool snapshot_metadata_valid{false};
    bool contains_snapshot{false};

    bool has_snapshot() const
    {
        if (snapshot_metadata_valid) return contains_snapshot;
        return std::any_of(updates.begin(), updates.end(),
                           [](const auto& update) { return update.is_snapshot; });
    }
};

using MarketEvent =
    std::variant<MarketTrade, BboQuote, MarketDepthSnapshot, IncrementalBookBatch>;
