#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#elif defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#endif

namespace concurrency
{
// A relaxed spin for a producer that finds a full ring or a consumer that finds an empty one. On x86
// the pause instruction also keeps the core from speculating past the load it is waiting on, which is
// the difference between a spin that costs one core and a spin that costs the memory bus.
inline void spin_relax()
{
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

/*
-------------------------------------------
 SpscRing
-------------------------------------------

A bounded single-producer single-consumer ring. One thread calls try_push, one thread calls try_pop,
and neither ever blocks on a lock: each side owns a counter, and the only cross-thread communication is
publishing one counter to the other.

The two counters are deliberately duplicated. `write_local_` and `read_local_` are private to their own
thread and never contended; `write_` and `read_` are the published copies the other side reads. The
alternative, both threads loading the same atomic on every operation, makes every store by one thread
invalidate the cache line the other is loading.

Memory ordering is the whole contract, and it is two lines of code:

  * after constructing a value in a slot, the producer stores the new write index with release, which
    publishes the slot's contents along with the index;
  * the consumer loads the write index with acquire before reading a slot, which is what makes those
    contents visible to it, and stores the read index with release before the producer may reuse it.

Anything weaker would be a data race on the value rather than a lost update: the slot would be read
while it is still being written.

The two indices are on separate cache lines, for the same reason false sharing is worth avoiding
anywhere: a ring whose producer and consumer fields share a line measures the cache coherence protocol
rather than the hand-off.

T may be non-trivial, which is what makes this usable for a market data event: a slot is raw storage,
the producer constructs the value in place and the consumer destroys it after moving it out. That puts
the requirement on T that it is move-assignable, and on the caller that the ring is not destroyed while
either side is still running.
-------------------------------------------
*/
template <typename T>
class SpscRing
{
   public:
    // The capacity is rounded up to a power of two, so an index wrap is a mask instead of a modulo.
    explicit SpscRing(std::size_t capacity)
    {
        static_assert(std::is_move_assignable_v<T>, "SpscRing needs a move-assignable value type");
        if (capacity == 0) throw std::invalid_argument("an SPSC ring needs at least one slot");
        std::size_t rounded = 1;
        while (rounded < capacity) rounded <<= 1;
        capacity_ = rounded;
        mask_ = rounded - 1;
        slots_ = std::make_unique<Slot[]>(rounded);
    }

    // Destroys whatever is still in the ring. It must not run while a producer or consumer is active:
    // the ring owns the values inside it, and that is a property of the caller, not of this class.
    ~SpscRing()
    {
        const uint64_t end = write_.load(std::memory_order_acquire);
        for (uint64_t index = read_local_; index < end; ++index) object_at(index)->~T();
    }

    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer side. Returns false when the ring is full and leaves the value untouched, so a caller
    // can retry the same value after waiting for room.
    bool try_push(const T& value)
    {
        return try_emplace(value);
    }

    bool try_push(T&& value)
    {
        return try_emplace(std::move(value));
    }

    // Consumer side. Returns false when the ring is empty.
    bool try_pop(T& out)
    {
        const uint64_t read = read_local_;
        if (read == write_.load(std::memory_order_acquire)) return false;
        T* value = object_at(read);
        out = std::move(*value);
        // The consumer destroys before it publishes the new read index, so the producer's next write
        // into this slot is never a write onto a live object.
        value->~T();
        read_.store(read + 1, std::memory_order_release);
        read_local_ = read + 1;
        return true;
    }

    std::size_t capacity() const
    {
        return capacity_;
    }

    // How many values are waiting, plus the consumer's own progress. Only the consumer may call this:
    // it reads a counter the producer owns, which is an ordinary acquire load for the consumer and
    // nobody else's business at all.
    uint64_t consumer_occupancy() const
    {
        return write_.load(std::memory_order_acquire) - read_local_;
    }

   private:
    struct Slot
    {
        alignas(T) unsigned char bytes[sizeof(T)];
    };

    Slot& slot_at(uint64_t index)
    {
        return slots_[static_cast<std::size_t>(index) & mask_];
    }

    T* object_at(uint64_t index)
    {
        return std::launder(reinterpret_cast<T*>(slot_at(index).bytes));
    }

    template <typename U>
    bool try_emplace(U&& value)
    {
        const uint64_t write = write_local_;
        // The acquire load is what makes the consumer's destruction of this slot visible, so the
        // placement new below never lands on an object that still exists.
        if (write - read_.load(std::memory_order_acquire) == capacity_) return false;
        new (slot_at(write).bytes) T(std::forward<U>(value));
        write_.store(write + 1, std::memory_order_release);
        write_local_ = write + 1;
        return true;
    }

    std::unique_ptr<Slot[]> slots_;
    std::size_t capacity_{0};
    std::size_t mask_{0};
    // The producer's two fields and the consumer's two fields never share a cache line, so neither
    // side pays for the other side's stores.
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) uint64_t write_local_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
    alignas(64) uint64_t read_local_{0};
};
}  // namespace concurrency
