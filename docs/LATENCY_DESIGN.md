# Low-Latency Design

This document explains **why** each low-latency change in this project is faster. It is the design
record: the problem, the structure that was chosen, the mechanism that makes it pay, and how it was
proved to be behavior preserving.

Numbers are *not* restated here. Each section points at a row of the
[optimization ledger](PERFORMANCE.md#5-optimization-ledger), which is the single place where
measurements live, along with the measurement method itself. Sections here quote a figure only where
the figure *is* the motivation for the design.

## How to read this

Every section has the same five parts:

- **Problem** — the specific shape of the data or the code that costs time, with the observation that
  identified it.
- **Design** — what was built, in prose and in the smallest code that shows the idea.
- **Why it is faster** — the mechanism. This is the part worth carrying to another codebase.
- **Verification** — how it was shown not to change behavior.
- **Result** — what it moved, in one line, pointing at the ledger.

## 1. The line layer: `LineReader`

Ledger row: [Buffered line reader](PERFORMANCE.md#5-optimization-ledger).

### Problem

Every reader in `src/market/` used `std::getline` (or `gzgets`) per row. Both are per-character
loops: `std::getline` appends one byte at a time into a `std::string` and re-checks the delimiter at
each step. On the incremental depth file a row is about 76 bytes and there are 2.59M rows in the
first 200 MB, so that loop is the innermost thing in the whole program.

The identifying observation was an isolation measurement of raw reads only:

| Stage | Throughput |
|---|---:|
| Raw 1 MiB block reads only | 5.4 GB/s |
| Block reads plus `memchr` newline scan | 2.6 GB/s |
| `std::getline` into `std::string` | 1.7 GB/s |

Block reads are an order of magnitude faster than the parse that follows them, which says the file
system is not the problem. The gap between 2.6 and 1.7 GB/s is what the character loop costs.

### Design

`src/market/line_reader.*` pulls 1 MiB blocks and locates line boundaries with `memchr`, handing back
a `std::string_view` into the block:

```cpp
// The common case: a line that fits in the current block costs one memchr and no copy.
const void* newline = std::memchr(block_ + cursor_, '\n', filled_ - cursor_);
if (newline != nullptr)
{
    const char* begin = block_ + cursor_;
    line = std::string_view(begin, static_cast<std::size_t>(static_cast<const char*>(newline) - begin));
    cursor_ = static_cast<std::size_t>(static_cast<const char*>(newline) - block_) + 1;
    return true;
}
```

Two details make it more than a `memchr` wrapper:

- **A line that crosses a block boundary** is compacted to the front of the block before the next
  read, so an owned copy is only needed for a line larger than a whole block. Without this, every
  block boundary would need a special case in every caller.
- **The same interface serves `std::ifstream` and `gzFile`**, so all five readers could switch at
  once, which is also what let the comma-counting pre-pass in `split_csv(std::string)` be deleted:
  a `string_view` line does not need a reserve computed from a first scan.

### Why it is faster

The cost being removed is not "scanning bytes", it is **byte-at-a-time work with a loop-carried
dependency**. Three separate effects compound:

1. `memchr` is vectorised and processes a whole cache line per iteration, so the scan is memory-bandwidth
   bound instead of branch bound.
2. The per-byte append into a `std::string` disappears, along with its capacity checks.
3. No allocation per row: the returned view points into the block, which stays alive until the next
   block read.

Effect 3 turned out to be the largest single contributor to the end-to-end saving, and it was not
visible in the line-layer isolation table. It came from the *batching boundary* rather than the scan:
`read_batch` had been using `pending_row_ = std::move(row)`, and because the move steals the row's
buffer, the next `getline` had to allocate a fresh one. On this workload that is about 1.18M
allocate/free pairs. Replacing the move with `pending_row_.assign(view)` reuses the existing capacity
and removes them. **The profiling story pointed at parsing; the biggest win was allocation churn one
level up.**

### Verification

`tests/line_reader_test.cpp` covers LF, CRLF, empty lines, a missing trailing newline, a line larger
than one block, block-level batching, gzip parity, and line-for-line parity against
`data/v2/test_aggTrades_5k.csv`. End-to-end, `data/runtime/orders.csv` and `trades.csv` match the
pre-change build byte for byte and the full stdout summary is identical.

### Result

`-0.258 s (-16.0%)` paired on W1, nine pairs. In isolation the line layer itself is only `1.44x`
faster, which is the point: **the 16% came from the line layer plus the allocation it removed from
the layer above.**

## 2. The L2 book container: `TickLadder`

Ledger row: [`TickLadder` book container](PERFORMANCE.md#5-optimization-ledger). This is the largest
single change in the project and the most transferable one.

### Problem

`L2OrderBook` stored each side in a `std::pmr::map<PriceTick, uint64_t>`. Five facts about the feed
made that a bad fit, and none of them are obvious from the code:

| Observation | Consequence |
|---|---|
| More than 1000 live levels per side | a red-black tree is about 10 levels deep, `log2(1000)` |
| A live price span of about 20,000 ticks | the tree is also *wide*, so its nodes are spread over many cache lines |
| 1.71 level updates per batch | the per-access cost is paid on every event |
| `market_view` reads the best five levels per batch | another 5 walks per event |
| `amount == 0` removes a level | every update is a find-then-modify, never an append |

The fifth row is the one that matters most for choosing a structure. A feed that only ever appends
would favour the "sorted vector beats a tree" argument. This feed deletes as often as it writes, which
is what a sorted vector is worst at, and that is why the two ended up within 10% of each other instead
of one winning.

The measurement that forced the change was that the two *ordered* containers were nearly tied:

| Storage | ns per batch, applied to the same derived batches |
|---|---:|
| `std::pmr::map` | 156.8 |
| sorted `std::vector` | 1.02-1.11x faster than the map |
| **flat array indexed by tick** | **40.1** |

A tree and a vector being within 10% is the tell: **both are paying for the same shape — a level is
found by walking to it.** Removing the walk is what changes the order of magnitude, and that is the
whole idea.

### Design

The array is indexed by price tick, not by rank:

```
tick:        ... 12300   12301  12302 ... 12613   12614   12615  ...
quantity_:   ...  5000      0      0  ...    0    12100   90050  ...
                  ^                            ^      ^
              a level                       empty   a level   best bid
```

```cpp
bool descending_;                    // true = bids, where the best level is the highest tick
PriceTick base_{0};                  // tick of slot 0
std::size_t count_{0};               // number of populated slots
std::vector<uint64_t> quantity_;     // the array itself
PriceTick best_{kNoBest};            // best populated tick, or -1
```

Four decisions inside that small structure carry the whole design:

**1. `base_` shifts the numbering.** Prices start around 12,600, so indexing 0..12,600 would waste
12,600 slots. `slot_of(tick) = tick - base_` makes the array start wherever the first price was.

**2. `best_` holds a tick, not a slot index.** Growth shifts every slot index but changes no tick, so
storing the tick means `best_` survives a reallocation untouched. This is the kind of small decision
that only looks obvious after the growth path exists.

**3. Zero means empty.** A populated slot is `quantity_[slot] != 0`, so there is no separate presence
bitmap and `count_` is the only bookkeeping. It also matches the feed's own semantics, where an
amount of zero *is* the deletion message:

```cpp
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
```

**4. The `best_ == kNoBest` line is not redundant.** With `kNoBest = -1` and ticks always positive,
the ascending comparison would reject the first level ever inserted into an ask side
(`12615 < -1` is false), leaving `best_` stuck at -1 and every read broken. The explicit "no best
yet" branch is what makes the two directions share one code path.

### The read path: walking from the hint, never from the ends

`nth(index)` returns the index-th best level. Scanning the array from slot 0 would read 20,000 slots,
and `market_view` calls it five times per batch, so it starts at `best_` and walks toward worse
prices:

```cpp
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
                if (found == index) { tick = base_ + slot; quantity = quantity_[slot]; return true; }
                ++found;
            }
            if (slot == 0) break;
            --slot;
        }
    }
    else { /* mirror image, walking up to quantity_.size() */ }
    return false;
}
```

The cost is `O(index + empty slots crossed)`, and the second term is why the choice is safe: a real
book is dense near the touch, so the walk crosses a handful of slots. The measured figure is about
5.6 slots per call.

### One walk for the whole view

`nth()` turned out to be the wrong shape for `market_view`, which needs the touch *and* the summed depth
of the best five levels. Asking for each level separately restarts the walk from the best slot, so
producing one five-level view crossed the near-touch slots about five times over — roughly 28 slot
visits per batch to read 10 levels, with `nth(0)` paid twice.

`TickLadder::top_levels(levels)` answers both questions in one pass:

```cpp
TopLevels top_levels(std::size_t levels) const;   // { touch tick, touch quantity, summed depth }
```

It walks outward from the best slot, accumulating the first `levels` populated slots and recording the
first as the touch. The same view now costs about 12 slot visits instead of 28, and the caller got
simpler: `market_view` no longer needs `std::min(levels, size())`, because a walk that runs out of book
returns what it found.

Two details of the measurement are worth keeping. `read+parse` and `book-apply` were recorded in the
same paired runs as controls and both came back unchanged (468.5 -> 465.5 ns, 45.8 -> 45.6 ns), which is
what a change confined to one container's read path has to look like. And the effect sizes are lopsided
in a reassuring way: the stage moved `-28.5%` at `t = -25.3` while the end-to-end wall clock moved
`-5.9%` at `t = -2.61`. **A stage result that strong next to an end-to-end result that weak is the
expected pairing**, because the stage is about 11% of the event and nothing more.

### Keeping the hint valid: `advance_best_hint`

Removing the best level has to find the next one. Two designs are tempting and both are wrong:
scanning the whole array costs 20,000 reads whenever the touch moves, and assuming the neighbour is
populated is simply false.

The design that works is to walk from the slot that was just emptied, in the direction of worse
prices:

```cpp
void TickLadder::advance_best_hint()
{
    if (count_ == 0) { best_ = kNoBest; return; }

    std::size_t slot = slot_of(best_);
    if (descending_)
    {
        // The emptied slot was the best, so every populated level is at a lower tick.
        while (slot > 0)
        {
            --slot;
            if (quantity_[slot] != 0) { best_ = base_ + slot; return; }
        }
    }
    else { /* mirror image */ }
    best_ = kNoBest;
}
```

The direction is correct **because** the emptied slot was the best: every other populated slot lies
on the worse side of it, so the walk cannot miss one. The cost is the gap to the next real level,
which on a dense book is one or two slots; only an almost-empty side walks far. Writing the hint on
the update path rather than in a `const` read path also keeps reads pure, which matters because reads
are called from a `const` method the compiler is free to reason about.

### Growth: learning the price window instead of configuring it

This is the decision that removed a configuration question. An incremental feed can deliver a deep
buy order at $1,000 and an ask at $21,000, so any fixed window is either wasteful or lossy. The
ladder derives its range from the levels themselves and grows when an update falls outside:

```cpp
void TickLadder::grow_to_cover(PriceTick tick)
{
    constexpr std::size_t kInitialSlots = 2048;
    constexpr std::size_t kGrowthMargin = 1024;
    ...
    new_base = std::min(base_, tick) - kGrowthMargin;
    const PriceTick new_top = std::max(old_top, tick + 1) + kGrowthMargin;
    ...
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
```

Three properties are worth pulling out:

- **It cannot lose an update.** Every arriving tick is either already representable or triggers a
  growth that makes it representable, so there is no "outside the window, drop it" branch and no cold
  fallback path. The *absence* of that branch is why this is simpler than a fixed-window design that
  has to handle overflow.
- **The margin is what makes it affordable.** Growing to exactly cover the new tick would reallocate
  on every consecutive out-of-range update. A 1024-slot margin means a drifting market pays nothing,
  and on this feed the range settles within the first few batches.
- **The shift is why `best_` holds a tick.** Old slot `s` lands at `s + (base_ - new_base)` and its
  tick is unchanged, so the hint needs no fixup. Storing an index would have required one here.

The guard `if (new_base < 0) { new_slots += -new_base; new_base = 0; }` exists because `slot_of` and
`in_range` both do unsigned arithmetic on `tick - base_`, and a negative base would wrap those
comparisons into always-true. Ticks come from positive prices, so this should never fire, but the cost
of being wrong is an out-of-bounds write, so the guard stays.

### Why it is faster

The mechanism is the removal of a **dependent load chain**, not the removal of instructions:

| Storage | What a level access costs |
|---|---|
| `std::pmr::map` | 10 dependent pointer loads, `log2(1000)` |
| sorted `std::vector` | a binary search, then `memmove` of up to 8 KB on insert |
| `TickLadder` | `tick - base_`, one load, one store |

The first two are *walks*: each step's address depends on the previous step's result, so the CPU
cannot start the next load until the last one returns. When the steps land in different cache lines,
the cost is the latency of each miss, in series. The array replaces the chain with an address
computation and a single load that the out-of-order engine can issue early and overlap with other
work.

The trade is memory: 8 bytes per slot over the live range is about 320 KB per side, against roughly
48 KB of tree nodes for 1000 levels. That looks like a poor trade until you look at *where* the
accesses land. The book is dense near the touch and the code only ever reads outward from `best_`, so
the working set is the few cache lines around the touch rather than the whole 320 KB: the memory is
allocated but not touched. **Sparse access into a large flat array is not the same as touching a
large flat array.**

### Verification

The reference for the new container is **the implementation it replaced**.
`tests/tick_ladder_test.cpp` applies random set and remove operations and compares size, best and a
sample of levels against a `std::map`, so the old behaviour is the specification rather than a
restatement of my own understanding of it. It also asserts the properties the map never had to state
explicitly: inserting far outside the initial range, walking across empty slots when the best is
removed, `nth` one past the end returning false, and `clear` resetting the hint.

Then the two storage implementations were compared on real data, which is the check that matters most:

| Check | Result |
|---|---|
| 117,288 real batches: level checksum and both depth totals | identical |
| Full 1.5 GB incremental file, 11,403,032 batches, `l2_replay` stdout | byte identical |

### Result

`book-apply` p50 `193 -> 51`, and the book in isolation `1085.3 -> 804.3 ns/batch` while producing the
same checksum. End-to-end the paired effect is `-9.5%` on the 200k slice, `-6.0%` on the 2M slice and
`-6.8%` on the full file by CPU time. **The gap between the 70% stage improvement and the 6-10%
end-to-end effect is the lesson**: a stage that is 16% of the event cannot return more than 16%, and
making it ten times faster there would still only return 16%.

## 3. The engine's per-symbol state: symbol ids

Ledger row: [Symbol id registry](PERFORMANCE.md#5-optimization-ledger).

### Problem

`MatchingEngine` keeps six containers keyed by symbol string:

```cpp
std::unordered_map<std::string, MEOrderBook> books_;
std::unordered_map<std::string, BboQuote> external_bbo_;
std::unordered_map<std::string, uint64_t> last_bid_trade_ts_;
std::unordered_map<std::string, uint64_t> last_ask_trade_ts_;
std::unordered_map<std::string, BboQuote> l2_top_bbo_;
std::unordered_set<std::string> queue_ahead_symbols_;
```

A single L2 batch reaches about ten of them, all with the same 13-character symbol, and each touch
hashes the whole string.

The shape to notice is the mismatch: **the engine's state is per symbol, but a run touches one symbol
millions of times.** Every event therefore pays to re-derive, over and over, something that never
changes.

### Design

A registry assigns each symbol a small integer the first time it appears, and the six containers key
on that:

```cpp
using SymbolId = uint32_t;
inline constexpr SymbolId kUnknownSymbol = 0;

// Returns the id for `symbol`, assigning one on first sight.
SymbolId symbol_id(const std::string& symbol)
{
    const auto [entry, inserted] = symbol_ids_.try_emplace(symbol, next_symbol_id_);
    if (inserted) ++next_symbol_id_;
    return entry->second;
}
```

The public interface keeps taking strings, so nothing outside the class changes. Each entry point
resolves once and passes the id inward, which is why the body of `process_bbo` became a separate
private method:

```cpp
void MatchingEngine::process_bbo(const BboQuote& quote)
{
    ++event_counter_;
    apply_pending_cancels();
    if (!is_usable_quote(quote)) { ...; return; }
    process_bbo_for(symbol_id(quote.symbol), quote);   // resolved here, once
}

void MatchingEngine::process_l2_top(const BboQuote& quote)
{
    ...
    const SymbolId id = symbol_id(quote.symbol);
    l2_top_bbo_[id] = quote;
    if (is_usable_quote(quote)) process_bbo_for(id, quote);   // handed over, not re-resolved
    ...
}
```

Two lookup variants exist on purpose:

- `symbol_id()` assigns an id, and is used where the event will create state anyway.
- `find_symbol()` returns `kUnknownSymbol` without registering, and is used where the symbol is only
  being looked up, so a *rejected* event does not grow the registry. The invalid-quote path is the
  example: it erases a stored quote if one exists, and a symbol that was never seen has nothing to
  erase, so it must not be added.

### Why it is faster

The removed cost is string hashing, and the size of that cost is a **mechanism question**, which is the
one thing an isolation benchmark answers well:

```
string  key: find(s) then [s] = v   23.8 ns     2 hashes
string  key: find(s), reuse iterator 13.7 ns     1 hash
integer key: find(k) then [k] = v   10.5 ns
```

The 10.1 ns between the first two lines is one hash of a 13-character string, and it also settles
something the source cannot show: `operator[]` re-hashes the key it was just given rather than reusing
the hash `find` already computed. That is why `find` + `operator[]` costs twice a single hash, and why
the idiom is worth avoiding on a hot path whether or not the key is an integer.

Ten string lookups become one string lookup plus ten integer lookups. The measured stage saving is
about half what the isolation numbers predict, and the reason is itself instructive: the benchmark
measures a **dependency chain**, where each iteration waits for the previous hash, while the real loop
has independent work around the lookups that the out-of-order engine overlaps with the hash.

### A correctness bug found on the way

The original body read the previous quantities *after* the store into the container:

```cpp
external_bbo_[quote.symbol] = quote;      // can rehash the container
...
apply_quantity_change(..., has_previous ? previous_quote->second.bid_quantity : 0);  // iterator!
```

`previous_quote` is an iterator into that container. Assigning through `operator[]` cannot rehash when
the key already exists, so the dereference was in practice safe, but only because of an invariant the
code did not state. The rewritten body hoists the two values it needs above the store, which removes
the latent dangling iterator at no cost:

```cpp
const uint64_t previous_bid_quantity = has_previous ? previous_quote->second.bid_quantity : 0;
const uint64_t previous_ask_quantity = has_previous ? previous_quote->second.ask_quantity : 0;
```

### Verification

`ctest` green, W2 and W3 invariants exact, W1 exact on both slices, and the full 1.5 GB run's stdout
byte-identical to the pre-change output across 11,403,032 batches.

### Result

`engine` p50 `141 -> 111 (-21.3%)`, 5/5 paired runs in the same direction, `t = -2.85`. The mean moved
further, `-31.7%`, because the stage's rare stalls shrank too. End-to-end the effect is below this
host's noise floor: 30 ns of a ~1000 ns event is 3%.

## 4. Measurement itself: `StageProfiler`

Ledger row: none, this is the instrument. See
[stage timing](PERFORMANCE.md#35-stage-timing).

### Problem

`gprof` needs a separate build preset and instruments every function, which changes the thing being
measured. More importantly, gprof answers "which functions are hot", while the questions that mattered
here were "which *phase* of an event is hot" and "how do the phases rank". Function-level data was too
fine-grained for that: several phases share no single function, so eight regions of one event had to be
timed directly.

### Design

`src/utils/stage_profiler.*` names those eight regions and records them per event. Three details are
what make the output trustworthy:

- **Sampling.** Only every 64th event is timed, so the instrument's own cost lands on 1.6% of events.
  The interval is a command-line flag, and a run without `--profile-stages` passes a `nullptr` and pays
  nothing beyond constructing the object.
- **Clock overhead is measured, not assumed.** Construction reads the clock twice and subtracts the
  minimum of those reads (35-50 ns on this host) from every sample. Without it each region would be
  charged for one clock read and the column sum would come out larger than the wall clock.
- **It reports percentiles, not just means.** This is what makes the output usable: a stage whose mean
  is inflated by one page-fault sample is still readable through its p50, and the p999 column *is* the
  stall distribution.

### Why the design of the instrument mattered

Two of its choices changed decisions, which is why it earns a section:

1. **The mean is the wrong statistic for a stage with rare stalls.** `read+parse` reaches tens of
   microseconds at p999 against a 528 ns median. Those few samples move the mean by hundreds of
   nanoseconds. Ranking stages by mean would have aimed the work at the wrong one.
2. **A stage's share is a cap on what optimizing it can return.** `book-apply` was 16% of the event,
   so no container improvement could have returned more than 16% end to end. Computing that ceiling
   before starting is what kept the container work from being oversold, and it is exactly the
   arithmetic that was missing when the 3.9x isolation number was first read as an end-to-end
   prediction.

### Verification

`tests/stage_profiler_test.cpp` asserts that exactly every interval-th event is sampled, that stages
which never ran contribute nothing, that a multi-region event is recorded as the sum of its regions,
that the report names every stage and flags a run too short to sample, and that the measured clock
overhead is positive.

## 5. The number conversions

Ledger row: [Integer-plus-scale price parse](PERFORMANCE.md#5-optimization-ledger).

### Problem

The parse layer is 61% of an event, and two earlier attempts at it had both failed because both had been
tuning the instructions *around* the thing that actually cost time. What they missed is that
**libstdc++ defines `std::from_chars<double>` out of line**, in the shared library, so a call to it looks
to a function-level profile like a leaf with no body: the conversion was invisible to the instrument
being used to choose targets.

That call happens once per row to convert the price column, 2,000,000 times per run. The same was true
of the two timestamps and the size, which go through `std::from_chars<uint64>` three times per row, and
the cold path those functions carried.

An earlier version of this section explained the invisibility differently, as gprof accounting for only
half of `read+parse` with the other half in library code. That comparison was invalid — it set a
RelWithDebInfo `-pg` build against a Debug build across two sessions — and when the candidates were
counted directly they were all too small to matter. See
[section 6 of the performance reference](PERFORMANCE.md#6-where-the-remaining-time-is).

### Design

The feed spells prices as plain decimals and the book works in integer ticks, so the conversion can be
integer arithmetic. The fast path accepts only the shape it can reproduce exactly — optional sign, one
or more integer digits, an optional `.` plus at most five fraction digits, and a mantissa within 2^53 —
and everything else falls back to the general parser:

```cpp
const double magnitude = static_cast<double>(mantissa) / kPow10[decimals];
return negative ? -magnitude : magnitude;
```

### Why the result is bit-identical rather than merely close

The mantissa is exact in a `double` because it is below 2^53, `10^decimals` is exact, and IEEE division
is correctly rounded. So the quotient is the correctly rounded value of the exact decimal, which is
precisely what `std::from_chars` produces. **For every input the fast path accepts, the two
implementations return the same bits.**

That equivalence is what makes this measurement trustworthy: the full 1.5 GB replay produces
byte-identical output over 11,403,032 batches, so nothing downstream can differ, so **any timing
difference is the parse and only the parse.** There is no confound available.

### The ablation that over-reported by 4x

Before writing the fix, the same call was removed the crude way: replace `parse_double` with
`return 6000.0 + value.size()`, which keeps the control flow but destroys the prices. That measured
**-259 ns** on `read+parse`, `t = -22.3`.

The real fix measures **-65.5 ns**. The ablation was wrong by four times, and why is worth recording:
**collapsing every price to one of two values also collapsed the order book**, from thousands of levels
to two, shrinking the whole program's working set. The reader then ran faster because the change had
emptied the cache, not because the call was gone.

An ablation is only evidence if it changes *only* the thing under test. The reliable way to guarantee
that is not to make the ablated code cheap but to make its output identical — at which point the
ablation has become the fix.

### The same treatment for the integer fields

Once the price was dealt with, attribution pointed at `parse_uint64`, which gprof charges 16.0% of
application time: two 16-digit timestamps and a five digit size per row, 3.3 calls per row. An
isolation bench on the real formats showed the shape of the cost rather than the digits:

```
from_chars + the inlined throwing path   26.3-28.2 ns
the same, with the cold path split out   21.9-24.1 ns
two digits per step, validation folded   13.4-14.3 ns
```

Two independent mechanisms, and both were worth taking:

- **The cold path belongs in its own function.** Constructing the error string and throwing were
  reachable from the hot function, so the hot path carried them as inlined code. Moving them into a
  `[[noreturn]]` helper alone was worth about 15%.
- **Two digits per step.** Reading both digits with independent loads and folding the validation into
  the same loop removes half the iterations and all the separate checking passes.

Equivalence is again the reason the result is trustworthy. A value of at most 18 digits cannot exceed
2^64-1, so the loop needs no overflow check, and because it validates every character it consumes it
accepts exactly what `std::from_chars` accepts and returns the same value. Anything longer, or empty,
goes through the general parser, which keeps the previous behaviour for the lengths that could
overflow. The full-file output is byte-identical, so the only thing that changed is cost.

### Verification

| Check | Result |
|---|---|
| W1 full file, 11,403,032 batches, stdout | byte identical |
| W1 200k slice, L2 invariants | exact |
| W2, L1 invariants | exact |
| `ctest` | 15/15 |
| New compiler warnings | none |

### Result

The price conversion: `read+parse` p50 `-65.5 ns (-11.3%)`, 5/5 pairs, `t = -5.9`. Reproduced at
`-71 ns` in a second session twenty minutes later, while the host was 1.4x slower: the sign and
magnitude held, the absolutes did not. End-to-end wall clock `-7.6%`, `t = -2.83`.

The integer conversions: `read+parse` p50 `-44.8 ns (-9.2%)`, 5/5 pairs, `t = -6.2`; end-to-end wall
clock `-6.0%`, `t = -4.49`.

`engine` and `book-apply` were unchanged in both same-session comparisons, which is what a change
confined to the reader has to look like. And the two results triangulate with the attribution that
motivated them: gprof charged `parse_uint64` 16% of application time, the isolation bench showed the
two-digit loop about 47% faster, and the end-to-end effect came out at 6%.

## 6. Designs that were measured and rejected

These matter as much as the accepted ones, because each was expected to win. Keeping them written down
is what stops the same idea being tried twice.

### A sorted `std::vector` for the book

The reasoning was that a tree's pointer chasing must be the problem, and that a contiguous sorted
array would beat it. Measured against the same derived batches, the vector was `1.02-1.11x` faster than
the `pmr::map` — effectively a tie, and nowhere near the `3.9x` the flat array delivered.

The tie is the informative part. **Both containers find a level by walking to it**: the tree walks
pointers, the vector walks a binary search and then `memmove`s on insert. They are called "different
data structures" but they share the shape that actually costs time. This feed deletes as often as it
writes, which is precisely the case where the vector's `memmove` cancels its lookup advantage, so the
two drawbacks offset.

Lesson: when two very different structures measure within 10% of each other, neither structure is the
problem — look for the property they share.

### Fixed-point price parsing — rejected first, then vindicated

`parse_uint64` and two `std::from_chars<double>` calls per row were identified as the largest single
item in the whole run, near a third of the per-event cost, so the plan was to parse prices into integer
ticks without a floating-point step. That plan was **rejected on an isolation comparison**:
`from_chars<double>` measured `16.6-17.3 ns` on `"6376.5"` while a hand-written integer path measured
`9.5-10.1 ns`. A 1.7x gain looked too small to justify the code.

**The rejection was wrong, and by about three times.** In context the call costs roughly `48 ns`, not
`17`, because the isolated benchmark feeds it one fixed string: the parser's internal branches are
perfectly predicted and its code stays resident in the instruction cache, and neither is true when the
column varies in length and digit count across two million rows. Reimplemented as an exactly equivalent
integer path and measured with the output held byte-identical, it is worth `-11%` of `read+parse` and
now lives in [section 5](#5-the-number-conversions).

Two lessons, and they are different:

- **A rejection needs an in-context measurement too.** "Only 1.7x" was a decision made from the one
  kind of measurement that cannot support it. What it threw away was the largest single remaining win
  in the program.
- **Isolation benchmarks are optimistic about branchy parsing specifically.** A fixed input makes every
  branch predictable, so this class of benchmark systematically under-reports anything that branches on
  data — which is what parsing is.

### A word-at-a-time scan for the field split

`split_csv_views` was the next attribution candidate after the number conversions: gprof charges it 8.8%
of application time, and the earlier record claimed its cost was dominated by `memchr` call setup,
since `std::string_view::find(',')` emits a libc `memchr` call per field and the fields are about ten
bytes wide.

The mechanism bench, on the two row shapes that dominate the file, refutes that. All three variants
produce identical fields, checked before timing:

| Delimiter search | 76 byte row | 78 byte row |
|---|---:|---:|
| `std::string_view::find`, the current code | 60-63 ns | 54-60 ns |
| byte loop | 61-64 ns | 53-57 ns |
| word at a time (SWAR) | 53-56 ns | 47-49 ns |

A byte loop is **the same speed as `memchr`**, so the call is not the cost: this host's `memchr` is fast
even for ten byte inputs. Only the word-at-a-time scan wins, by about 12%, which is 8 ns per row or
about 1.5% of the event at 1.69 rows per event.

**1.5% is below the measurement floor**, so the change is not shipped. It is recorded because the
mechanism result matters twice over: it retires the `memchr` explanation, and it puts the field split
close to its own floor, which means the remaining `read+parse` cost is not in the delimiter search that
someone would otherwise tune next.

### Removing the book mutex

`L2OrderBook` holds a `std::mutex` around every access, and the replay is single threaded, so dropping
it looked free. Measured in isolation, an uncontended lock/unlock pair costs `4.2 ns` of a ~1200 ns
event, or `0.7%`.

Not worth it. An uncontended `std::mutex` is a handful of atomic instructions on a cache line the
thread already owns, and 0.7% does not buy back the loss of thread safety — or the cost of adding a
wrapper type to restore it later.

Lesson: an uncontended lock is not "a lock". Price it before designing around it.

### Reader micro-optimizations

The inline four-slot update buffer, the column-role jump table and the `parse_boolean` first-character
dispatch were all built and verified, then measured with ABBA pairing over four blocks: a paired mean
of `+5.4 ns` with a standard error of `12.4 ns` on a ~530 ns stage. **Indistinguishable from zero.**

They are kept because they are behavior-preserving, they do remove real work (one `malloc`/`free` per
batch among them), and reverting them would cost more verification than it returns. But the honest
label is *unproven*, and the useful conclusion is the one in the performance reference: the row loop's
micro-structure is not where the remaining 61% lives.

Lesson: "this removes work" and "this is measurable" are different claims. The second one needs a
number, and a null result is a result.

## 7. The transferable lessons

Ordered by how much they changed decisions here, not by how general they are.

**1. Look for dependent load chains, not instruction counts.** The two biggest wins came from removing
a *walk*: a tree descent replaced by an address computation, and a per-row character loop replaced by a
vectorised scan. In both cases the cost was not the number of instructions but that each step's address
depended on the previous step's result, leaving the CPU nothing to overlap. Before optimizing a hot
loop, ask which loads wait for which.

**2. A large flat array with sparse access is cheap.** `TickLadder` allocates about 320 KB per side to
hold a thousand live levels. It wins anyway, because the accesses cluster around the touch, so the
touched working set is a few cache lines. "Big array" and "big working set" are separate questions.

**3. Learn the range; do not configure it.** Making the book's price window a setting would have
created a permanent tuning problem (too small loses deep levels, too large wastes memory) and a
correctness cliff at the boundary. Deriving it from the data and growing on demand removed both, and it
also removed the "what do we do with an out-of-window update" branch entirely. **When a design needs a
configuration parameter, ask whether the data can supply it instead.**

**4. Maintain invariants on the write path so reads stay pure.** The best-level hint is updated only
in `set()`, never in a `const` reader, so `nth()` has no shared mutable state and no cache-writing
behaviour in a read path. This is also why `best_` stores a tick rather than a slot index: the
invariant (the best populated tick) is expressible without knowing the layout, so a reallocation cannot
break it.

**5. A stage's share caps what optimizing it can return.** Do that arithmetic before starting.
`book-apply` at 16% could never return more than 16%; the container became 70% faster inside the stage
and returned 6-10% end to end. Both numbers are correct and the second is the one that matters. An
isolation benchmark reports the first; only the stage table tells you the second.

**6. Use the implementation you are replacing as the test oracle.** `tick_ladder_test` compares against
a `std::map` over random operations, so the old behavior *is* the specification rather than my
restatement of it. A test written from the new code's logic cannot catch a misunderstanding — it shares
it. And for a container swap, the strongest check was not a unit test at all: it was diffing 11.4M
batches of real output.

**7. An ablation is only evidence if it changes only the thing under test.** Removing a call the crude
way — replace it with a constant — measured `-259 ns` for a change that is really worth `-65.5 ns`. The
gap was not noise: destroying the price column also collapsed the order book, which shrank the whole
program's working set and let the reader ride the emptied cache. **Make the ablated code cheap and you
have changed two things at once; make its output identical and you have measured one.**

**8. Predict, then measure, and expect the prediction to be optimistic.** Every cost estimate in this
project was wrong by 3-10x in the same direction, and the failures were the most informative events in
it. The ledger in the performance reference exists because of that bias: it records attempts, not wins.
The single estimate that was wrong in the *other* direction — a rejection that should have been an
acceptance — cost more than all of them combined.

**9. Verify behavior on the largest input available before claiming a number.** Every behavioral
guarantee in this document rests on a full-file comparison, not on a slice. A slice can hide a rare
path, and the rare path is exactly what a rewrite is most likely to break. The price-parse change is the
clearest case: its timing claim is only defensible *because* 11.4M batches of output are byte-identical,
which rules out every other explanation for the speedup.







