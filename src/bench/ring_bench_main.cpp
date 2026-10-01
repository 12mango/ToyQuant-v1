/*
-------------------------------------------
 ring_bench — what one message through the hand-off costs
-------------------------------------------

`[LATENCY]` reports what the hand-off cost inside a replay, where the feed and the strategy are also
doing their own work. This measures the hand-off on its own, and against the two alternatives a reader
would otherwise reach for, because "lock-free" is a claim about a mechanism rather than a number.

Three questions, each a row of the table this prints:

  * What does one message cost through `concurrency::SpscRing`, with a producer thread on one side and a
    consumer thread on the other?
  * Does the payload matter? A ring that copied bytes would answer yes; this one placement-news each
    value into a slot, so a trade event and a 64-bit integer should cost about the same, and the rows say
    whether that is true or merely intended.
  * What would the same hand-off cost with a mutex in it -- one that sleeps the consumer on a condition
    variable, which is the pattern most code reaches for, and one that spins on the lock, which separates
    the wake-up from the mutual exclusion?

Two smaller rows price the two ends of the ring: an empty round trip on a single thread, which is the
hand-off with no other thread in it at all, and a full ring, where every push is refused and the producer
waits, which is the branch a bounded hand-off takes instead of dropping an event.

The protocol is the honest one rather than the flattering one: one untimed pass to warm the machine, then
`repetitions` timed passes, and the report is the median of them with the lowest and highest pass beside it.
Medians because a microbenchmark's samples are not symmetric -- one scheduler preemption moves a mean and
does not move a median -- and the spread because it is the only uncertainty a benchmark can state: a median
quoted without one invites a reader to believe three significant figures on a machine with a turbo clock.

Timing numbers are not asserted by the gate, because a threshold on a microbenchmark is a threshold on
whatever else the machine was doing. What the gate does check is that the ring still carried the right
number of values: the `[RING_BENCH]` line carries `messages` and `checksum`, and
`docs/PERFORMANCE_HISTORY.md` re-derives them through `tools/check_docs.py`.

Usage:  ring_bench [messages=1000000] [repetitions=3]
Build:  cmake --build --preset linux-relwithdebinfo --target ring_bench
-------------------------------------------
*/

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "concurrency/spsc_ring.h"
#include "market/market_event.h"

namespace
{
using Clock = std::chrono::steady_clock;

// The capacity the replay's feed uses by default, so the streaming rows measure the shape a run has. The
// small-ring row is the same hand-off with a ring too small for the burst, which is backpressure.
constexpr std::size_t kRingCapacity = 4096;
constexpr std::size_t kSmallRingCapacity = 8;

double ns_between(Clock::time_point start, Clock::time_point end)
{
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

std::string cpu_model()
{
    std::ifstream info("/proc/cpuinfo");
    std::string line;
    while (std::getline(info, line))
    {
        const std::string key = "model name";
        if (line.compare(0, key.size(), key) != 0) continue;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string value = line.substr(colon + 1);
        const std::size_t first = value.find_first_not_of(" \t");
        if (first != std::string::npos) value = value.substr(first);
        // The name carries spaces and this ends up on a one-line summary, so it is folded to underscores.
        std::replace(value.begin(), value.end(), ' ', '_');
        return value;
    }
    return "unknown";
}

// The median of `repetitions` measured passes, each of which returns the total nanoseconds it took. The
// median is taken over passes rather than over operations: a median over interleaved samples would mix
// passes measured under different clock conditions. The low and high passes come with it, because on a
// machine with a turbo clock the spread across passes is the only uncertainty a benchmark can state, and a
// median quoted without it invites a reader to believe three significant figures.
struct PassStats
{
    double median{0.0};
    double low{0.0};
    double high{0.0};
};

template <typename Measure>
PassStats measure_passes(uint64_t messages, int repetitions, Measure measure)
{
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(repetitions));
    for (int pass = 0; pass < repetitions; ++pass)
        samples.push_back(measure() / static_cast<double>(messages));
    std::sort(samples.begin(), samples.end());
    return PassStats{samples[samples.size() / 2], samples.front(), samples.back()};
}

// A checksum of what crossed, so a ring that broke fails the gate rather than quietly measuring faster.
uint64_t mix(uint64_t seed, uint64_t value)
{
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

MarketEvent make_trade(uint64_t index)
{
    MarketTrade trade;
    trade.ts = index;
    trade.symbol = "BTC-PERPETUAL";
    trade.price = 7000.0 + static_cast<double>(index % 97);
    trade.quantity = 1 + (index % 7);
    trade.aggressor_side = (index % 2 == 0) ? Side::Buy : Side::Sell;
    trade.sequence = index;
    trade.exchange = "deribit";
    return trade;
}

// An incremental batch with two level changes, which is what a real Deribit incremental message carries
// and what the replay's ring actually moves. Two updates fit inside IncrementalUpdates' inline capacity,
// so the payload needs no heap allocation and the row measures the hand-off rather than an allocator.
MarketEvent make_batch(uint64_t index)
{
    IncrementalBookBatch batch;
    batch.ts = index;
    batch.exchange_ts = index;
    batch.local_ts = index + 1;
    batch.symbol = "BTC-PERPETUAL";
    batch.exchange = "deribit";
    const double price = 7000.0 + static_cast<double>(index % 97);
    batch.updates = {{index, index + 1, false, Side::Buy, price, 1 + (index % 7)},
                     {index, index + 1, false, Side::Sell, price + 0.5, 1 + (index % 5)}};
    return batch;
}

uint64_t make_uint64(uint64_t index)
{
    return index * 2654435761u;
}

// Three control payloads for the size question: 16 trivially copyable bytes, 128 of them, and one that is
// exactly as many bytes as a MarketEvent. None of them is a market event; between them they answer whether
// the payload rows are measuring the payload's size and the cache lines it occupies, or something that is
// specific to a variant carrying strings -- which is the difference between "a smaller event would be
// cheaper" and "a trivial event of the same size would be cheaper", and those are different findings.
struct Pod16
{
    uint64_t first;
    uint64_t second;
};

struct Pod128
{
    uint64_t words[16];
};

struct PodEventSized
{
    alignas(alignof(MarketEvent)) uint64_t words[(sizeof(MarketEvent) + 7) / 8];
};

static_assert(sizeof(PodEventSized) == sizeof(MarketEvent),
              "the control payload must be exactly the event's size for the comparison to hold");

Pod16 make_pod16(uint64_t index)
{
    return Pod16{index, index * 3};
}

Pod128 make_pod128(uint64_t index)
{
    Pod128 value{};
    for (int word = 0; word < 16; ++word) value.words[word] = index + static_cast<uint64_t>(word);
    return value;
}

PodEventSized make_pod_event_sized(uint64_t index)
{
    PodEventSized value{};
    for (std::size_t word = 0; word < sizeof(value.words) / sizeof(value.words[0]); ++word)
        value.words[word] = index + word;
    return value;
}

// ---------------------------------------------------------------- the cases

// Single thread: a push and a pop with nothing to wait for and no other core involved. This is the cost
// of the mechanism itself rather than the cost of moving a cache line between two cores.
template <typename T, typename Make>
double empty_round_trip(uint64_t messages, Make make, uint64_t* checksum)
{
    concurrency::SpscRing<T> ring(kRingCapacity);
    T value = make(0);
    T out = make(0);
    uint64_t sum = 0;
    const auto start = Clock::now();
    for (uint64_t index = 0; index < messages; ++index)
    {
        value = make(index);
        while (!ring.try_push(std::move(value))) concurrency::spin_relax();
        while (!ring.try_pop(out)) concurrency::spin_relax();
        sum = mix(sum, index);
    }
    const auto end = Clock::now();
    *checksum = sum;
    return ns_between(start, end);
}

// The producer's side of a full ring: every push is refused, which is what a bounded hand-off pays for
// backpressure instead of dropping an event.
double full_ring_push(uint64_t messages, uint64_t* refusals)
{
    concurrency::SpscRing<uint64_t> ring(kRingCapacity);
    for (std::size_t slot = 0; slot < kRingCapacity; ++slot)
        ring.try_push(static_cast<uint64_t>(slot));
    uint64_t refused = 0;
    const auto start = Clock::now();
    for (uint64_t index = 0; index < messages; ++index)
    {
        if (ring.try_push(index)) continue;
        ++refused;
    }
    const auto end = Clock::now();
    *refusals = refused;
    return ns_between(start, end);
}

// Two threads: the producer fills, the consumer empties, and every value crosses a cache line as well as
// the ring. A full ring is backpressure here too -- the same value is retried and never dropped -- and the
// waits are counted, so a row that is slow because it waited says so rather than looking like overhead.
//
// `produce` is called once per message, which is what makes the difference between the rows visible: a
// function that builds a trade from an index pays for building it, and a lambda that returns a prototype
// pays only for copying it into the slot.
template <typename T, typename Produce>
double stream_two_threads(uint64_t messages, std::size_t capacity, Produce produce, uint64_t* waits,
                          uint64_t* checksum)
{
    concurrency::SpscRing<T> ring(capacity);
    std::atomic<bool> producer_ready{false};
    std::atomic<bool> start{false};
    uint64_t push_waits = 0;
    std::thread producer(
        [&]
        {
            producer_ready.store(true, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) concurrency::spin_relax();
            for (uint64_t index = 0; index < messages; ++index)
            {
                T value = produce(index);
                while (!ring.try_push(std::move(value)))
                {
                    ++push_waits;
                    concurrency::spin_relax();
                }
            }
        });

    while (!producer_ready.load(std::memory_order_acquire)) concurrency::spin_relax();
    T out = produce(0);
    uint64_t sum = 0;
    start.store(true, std::memory_order_release);
    const auto begin = Clock::now();
    for (uint64_t index = 0; index < messages; ++index)
    {
        while (!ring.try_pop(out)) concurrency::spin_relax();
        sum = mix(sum, index);
    }
    const auto end = Clock::now();
    producer.join();
    *waits = push_waits;
    *checksum = sum;
    return ns_between(begin, end);
}

// The pattern most code reaches for: a mutex and a condition variable, with the consumer sleeping until
// the producer wakes it. Unbounded on purpose, because that is the version nobody has to think about.
template <typename T, typename Make>
double stream_mutex_condvar(uint64_t messages, Make make)
{
    std::deque<T> queue;
    std::mutex mutex;
    std::condition_variable ready;
    std::atomic<bool> producer_ready{false};
    std::atomic<bool> start{false};
    std::thread producer(
        [&]
        {
            producer_ready.store(true, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) concurrency::spin_relax();
            for (uint64_t index = 0; index < messages; ++index)
            {
                T value = make(index);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    queue.push_back(std::move(value));
                }
                ready.notify_one();
            }
        });

    while (!producer_ready.load(std::memory_order_acquire)) concurrency::spin_relax();
    T out = make(0);
    start.store(true, std::memory_order_release);
    const auto begin = Clock::now();
    for (uint64_t index = 0; index < messages; ++index)
    {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [&] { return !queue.empty(); });
        out = std::move(queue.front());
        queue.pop_front();
    }
    const auto end = Clock::now();
    producer.join();
    return ns_between(begin, end);
}

// The same queue with the sleeping taken out: the consumer spins on the lock instead. This separates the
// cost of the wake-up from the cost of mutual exclusion, which is the part a lock-free ring avoids.
template <typename T, typename Make>
double stream_mutex_spin(uint64_t messages, Make make, uint64_t* empty_checks)
{
    std::deque<T> queue;
    std::mutex mutex;
    std::atomic<bool> producer_ready{false};
    std::atomic<bool> start{false};
    std::thread producer(
        [&]
        {
            producer_ready.store(true, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) concurrency::spin_relax();
            for (uint64_t index = 0; index < messages; ++index)
            {
                T value = make(index);
                std::lock_guard<std::mutex> lock(mutex);
                queue.push_back(std::move(value));
            }
        });

    while (!producer_ready.load(std::memory_order_acquire)) concurrency::spin_relax();
    T out = make(0);
    uint64_t empty = 0;
    start.store(true, std::memory_order_release);
    const auto begin = Clock::now();
    for (uint64_t index = 0; index < messages; ++index)
    {
        while (true)
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (queue.empty())
            {
                ++empty;
                concurrency::spin_relax();
                continue;
            }
            out = std::move(queue.front());
            queue.pop_front();
            break;
        }
    }
    const auto end = Clock::now();
    producer.join();
    *empty_checks = empty;
    return ns_between(begin, end);
}
}  // namespace

int main(int argc, char** argv)
{
    uint64_t messages = 1000000;
    int repetitions = 3;
    if (argc >= 2) messages = std::stoull(argv[1]);
    if (argc >= 3) repetitions = std::stoi(argv[2]);
    if (messages == 0 || repetitions < 1)
    {
        std::fprintf(stderr, "usage: ring_bench [messages=1000000] [repetitions=3]\n");
        return 2;
    }

    struct Row
    {
        const char* key;
        const char* label;
        PassStats stats;
        uint64_t waits;
    };
    std::vector<Row> rows;

    // Every case folds the same index sequence into a checksum, so the value is a property of how many
    // values crossed rather than of which case crossed them.
    uint64_t checksum = 0;
    uint64_t waits = 0;

    // One untimed pass first, so the first timed pass is not the one that pays for the ramp-up of the two
    // cores and of the queue's first allocations.
    uint64_t warmup_checksum = 0;
    stream_two_threads<MarketEvent>(messages / 10, kRingCapacity, make_trade, &waits,
                                    &warmup_checksum);

    uint64_t round_trip_checksum = 0;
    // One event built once and copied into every slot, so the payload rows below can separate what building
    // a payload costs from what moving it costs.
    const MarketEvent prototype_trade = make_trade(1);
    const Pod16 prototype_pod16 = make_pod16(1);
    const Pod128 prototype_pod128 = make_pod128(1);
    const PodEventSized prototype_pod_event = make_pod_event_sized(1);

    rows.push_back({"empty_round_trip", "empty round trip, 1 thread, uint64",
                    measure_passes(messages, repetitions,
                                     [&]
                                     {
                                         return empty_round_trip<uint64_t>(messages, make_uint64,
                                                                          &round_trip_checksum);
                                     }),
                    0});
    checksum = round_trip_checksum;

    rows.push_back({"full_ring_push", "full ring, refused push",
                    measure_passes(messages, repetitions,
                                     [&] { return full_ring_push(messages, &waits); }),
                    waits});

    rows.push_back({"stream_uint64", "stream uint64, 2 threads, ring 4096",
                    measure_passes(messages, repetitions,
                                     [&]
                                     {
                                         return stream_two_threads<uint64_t>(messages, kRingCapacity,
                                                                             make_uint64, &waits, &checksum);
                                     }),
                    waits});

    rows.push_back({"stream_pod16", "stream 16 byte POD, prebuilt, 2 threads, ring 4096",
                    measure_passes(messages, repetitions,
                                   [&]
                                   {
                                       return stream_two_threads<Pod16>(
                                           messages, kRingCapacity,
                                           [&](uint64_t) { return prototype_pod16; }, &waits, &checksum);
                                   }),
                    waits});

    rows.push_back({"stream_pod128", "stream 128 byte POD, prebuilt, 2 threads, ring 4096",
                    measure_passes(messages, repetitions,
                                   [&]
                                   {
                                       return stream_two_threads<Pod128>(
                                           messages, kRingCapacity,
                                           [&](uint64_t) { return prototype_pod128; }, &waits, &checksum);
                                   }),
                    waits});

    rows.push_back({"stream_pod_event",
                    "stream POD the event's size, prebuilt, 2 threads, ring 4096",
                    measure_passes(messages, repetitions,
                                   [&]
                                   {
                                       return stream_two_threads<PodEventSized>(
                                           messages, kRingCapacity,
                                           [&](uint64_t) { return prototype_pod_event; }, &waits,
                                           &checksum);
                                   }),
                    waits});

    rows.push_back({"stream_trade", "stream trade event, built per message, 2 threads, ring 4096",
                    measure_passes(messages, repetitions,
                                     [&]
                                     {
                                         return stream_two_threads<MarketEvent>(messages, kRingCapacity,
                                                                                make_trade, &waits, &checksum);
                                     }),
                    waits});

    rows.push_back({"stream_trade_prebuilt", "stream trade event, prebuilt, 2 threads, ring 4096",
                    measure_passes(messages, repetitions,
                                     [&]
                                     {
                                         return stream_two_threads<MarketEvent>(
                                             messages, kRingCapacity,
                                             [&](uint64_t) { return prototype_trade; }, &waits, &checksum);
                                     }),
                    waits});

    rows.push_back({"stream_batch", "stream book batch, built per message, 2 threads, ring 4096",
                    measure_passes(messages, repetitions,
                                     [&]
                                     {
                                         return stream_two_threads<MarketEvent>(messages, kRingCapacity,
                                                                                make_batch, &waits, &checksum);
                                     }),
                    waits});

    rows.push_back({"small_ring", "stream uint64, 2 threads, ring 8",
                    measure_passes(messages, repetitions,
                                     [&]
                                     {
                                         return stream_two_threads<uint64_t>(messages, kSmallRingCapacity,
                                                                             make_uint64, &waits, &checksum);
                                     }),
                    waits});

    rows.push_back({"mutex_condvar", "stream trade event, mutex + condvar",
                    measure_passes(messages, repetitions,
                                     [&]
                                     { return stream_mutex_condvar<MarketEvent>(messages, make_trade); }),
                    0});

    rows.push_back({"mutex_spin", "stream trade event, mutex, consumer spins",
                    measure_passes(messages, repetitions,
                                     [&]
                                     {
                                         return stream_mutex_spin<MarketEvent>(messages, make_trade,
                                                                               &waits);
                                     }),
                    waits});

    const std::string cpu = cpu_model();
    std::printf("ring_bench messages=%llu repetitions=%d payload_bytes=%zu pod_bytes=%zu cpu=%s build=%s\n",
                static_cast<unsigned long long>(messages), repetitions, sizeof(MarketEvent),
                sizeof(Pod128), cpu.c_str(),
#ifdef NDEBUG
                "NDEBUG"
#else
                "assertions on, still -O2 -march=native"
#endif
    );
    std::printf("%-42s %9s %13s %10s\n", "case", "ns/op", "spread", "M ops/s");
    for (const Row& row : rows)
    {
        std::printf("%-42s %9.2f %6.2f-%-6.2f %10.2f\n", row.label, row.stats.median, row.stats.low,
                    row.stats.high, row.stats.median > 0.0 ? 1000.0 / row.stats.median : 0.0);
        if (row.waits > 0)
            std::printf("%-42s %9s %13s %10llu\n", "  (refused pushes or empty checks)", "", "",
                        static_cast<unsigned long long>(row.waits));
    }

    // The line the gate reads. `messages` and `checksum` are deterministic and are what
    // docs/PERFORMANCE_HISTORY.md re-derives; the nanosecond fields are what the same page quotes, and they
    // are a property of the machine that ran it rather than of the code.
    std::printf("[RING_BENCH] messages=%llu repetitions=%d checksum=%llu payload_bytes=%zu",
                static_cast<unsigned long long>(messages), repetitions,
                static_cast<unsigned long long>(checksum), sizeof(MarketEvent));
    for (const Row& row : rows) std::printf(" %s=%.2f", row.key, row.stats.median);
    std::printf(" cpu=%s\n", cpu.c_str());
    return 0;
}
