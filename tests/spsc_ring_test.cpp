// The ring's contract: one producer, one consumer, no lock, and no value lost or reordered. The
// two-thread case is the part that cannot be established by reading the atomics: it either delivers
// every value in order or it does not.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/spsc_ring.h"

namespace
{
std::atomic<int> live{0};

// A value that counts itself, so the ring's destructor path is observable: the slots hold constructed
// objects, and one left in a ring that is destroyed is a leak in a place a test would not otherwise
// look.
struct Tracked
{
    int value{0};

    explicit Tracked(int initial) : value(initial)
    {
        live.fetch_add(1, std::memory_order_relaxed);
    }
    Tracked(const Tracked& other) : value(other.value)
    {
        live.fetch_add(1, std::memory_order_relaxed);
    }
    Tracked(Tracked&& other) noexcept : value(other.value)
    {
        live.fetch_add(1, std::memory_order_relaxed);
    }
    Tracked& operator=(const Tracked& other)
    {
        value = other.value;
        return *this;
    }
    Tracked& operator=(Tracked&& other) noexcept
    {
        value = other.value;
        return *this;
    }
    ~Tracked()
    {
        live.fetch_sub(1, std::memory_order_relaxed);
    }
};
}  // namespace

int main()
{
    {
        concurrency::SpscRing<int> ring(3);
        // A capacity is rounded up to a power of two so the index wrap is a mask.
        assert(ring.capacity() == 4);
        int out = 0;
        assert(ring.consumer_occupancy() == 0);
        assert(!ring.try_pop(out));
        assert(ring.try_push(1) && ring.try_push(2) && ring.try_push(3) && ring.try_push(4));
        // Full: refused, and the caller's value is left alone so it can be retried.
        assert(!ring.try_push(5));
        assert(ring.consumer_occupancy() == 4);
        for (int expected = 1; expected <= 4; ++expected)
        {
            assert(ring.try_pop(out));
            assert(out == expected);
        }
        assert(!ring.try_pop(out));
        assert(ring.consumer_occupancy() == 0);
    }
    {
        // Wrap-around, which is where a mask instead of a modulo and a pair of local counters either
        // work or produce values from the previous lap.
        concurrency::SpscRing<std::string> ring(2);
        std::string out;
        for (int index = 0; index < 100; ++index)
        {
            const std::string value = "event-" + std::to_string(index);
            assert(ring.try_push(value));
            assert(ring.try_pop(out));
            assert(out == value);
        }
    }
    {
        // A ring that is destroyed with values still in it destroys them, and it destroys the value a
        // move left behind rather than a second live object.
        assert(live.load() == 0);
        {
            concurrency::SpscRing<Tracked> ring(4);
            assert(ring.try_push(Tracked(1)));
            assert(ring.try_push(Tracked(2)));
            Tracked out(0);
            assert(ring.try_pop(out));
            assert(out.value == 1);
        }
        assert(live.load() == 0);
    }
    {
        // The two-thread case: a small ring on purpose, so the producer spends most of the run
        // waiting for room and the hand-off is exercised rather than raced through.
        constexpr int kCount = 100000;
        concurrency::SpscRing<std::string> ring(64);
        std::vector<std::string> received;
        received.reserve(kCount);
        std::thread consumer(
            [&]
            {
                std::string value;
                while (static_cast<int>(received.size()) < kCount)
                {
                    if (ring.try_pop(value)) received.push_back(std::move(value));
                    else concurrency::spin_relax();
                }
            });
        for (int index = 0; index < kCount; ++index)
        {
            const std::string value = std::to_string(index);
            while (!ring.try_push(value)) concurrency::spin_relax();
        }
        consumer.join();
        assert(static_cast<int>(received.size()) == kCount);
        for (int index = 0; index < kCount; ++index) assert(received[index] == std::to_string(index));
    }
    assert(live.load() == 0);
}
