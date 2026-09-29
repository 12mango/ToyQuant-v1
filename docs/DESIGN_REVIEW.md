# Design Review

`toy_quant` replays market data through the whole trading loop and reports what a market maker would
experience, with the queue in front of a quote modelled instead of assumed.

```text
Deribit L2 depth ─► L2OrderBook ─► L2MarketView ─┐
Deribit trades   ─► MarketEvent ────────────────┼─► Pipeline ─► Strategy ─► MatchingEngine ─► reports ─► Portfolio
legacy CSV / UDP / Binance ─────────────────────┘
```

| | |
|---|---|
| **Size** | 64 files, ~10k lines: `app` 2431, `market` 1910, `legacy` 1432, `exchange` 1085, `strategy` 1017, `orderbook` 645, `backtest` 467, `utils` 396, `accounting` 247, `common` 147 |
| **Tests** | 15 CTest binaries, one per module, run by tier 0 of the gate |
| **Tools** | 18 scripts: the gate, the measurement sweeps, the queue calibration, two lints |
| **Build** | CMake + presets, C++20, standard library only (zlib for `.gz` inputs) |
| **Numbers** | every figure below is re-derived by `bash tools/verify_l2.sh fast` on every change |

**This is a demo, not a trading system.** No connectivity, no persistence, no risk gateway, no market impact.
The loop is meant to be readable in one sitting. What is worth reading is the engineering in sections 2 to 4.

## 1. The loop, in code

Three interfaces carry the design, each with one production implementation:

| Interface | Implementation | Why it is an interface |
|---|---|---|
| `Strategy` — `src/strategy/strategy.h` | one class per strategy name, built by `make_strategy` | the pipeline drives any decision module without knowing which |
| `IMatchingEngine` — `src/exchange/matching_engine.h` | `MatchingEngine` | reports arrive through a callback, so the pipeline never reaches into the engine |
| `IOrderBook` — `src/orderbook/orderbook.h` | `OrderBook`, `L2OrderBook` | the L1 and L2 paths share one pipeline |

One L2 event, top to bottom. `Pipeline::process_l2_market_view` (`src/app/pipeline.cpp`), trimmed:

```cpp
void Pipeline::process_l2_market_view(const L2MarketView& view)
{
    current_ts_ = view.ts;
    ...
    latest_quotes_.insert_or_assign(view.symbol, quote);

    engine_.process_l2_top(quote);              // the report callback fires inside this call

    // The report callback runs inside process_l2_top and reads last_mid_, so this
    // update has to stay after that call.
    last_mid_ = (view.top.bid_price + view.top.ask_price) / 2.0;
    ++quote_cycle_;
    while (!pending_markouts_.empty() && quote_cycle_ >= pending_markouts_.front().start_cycle + 5)
    { ... }                                     // five-cycle markout resolution

    submit_strategy_actions(view.symbol, view.ts, strategy_.on_l2_market_view(view));
    sample_resting_queues();                    // queue distribution: one lookup per open order
}
```

Two decisions are visible in that excerpt rather than in prose. **The strategy runs last**, so it sees the
book the engine just applied instead of the previous one. **The queue sampling is unconditional**, so the
distribution covers every order rather than only the ones a trace was asked for.

## 2. C++ decisions

| Decision | Where | Why | Measured effect |
|---|---|---|---|
| Blocked reader instead of `std::getline` / `gzgets` | `src/market/line_reader.h` | both stream a `std::string` and walk bytes; the feed is 1.5 GB of ~56-byte rows | line layer 0.115 → 0.080 s, within 10% of the raw `memchr` floor; end-to-end -16.0% |
| Flat array indexed by tick instead of `std::map` | `src/orderbook/tick_ladder.h` | a tree descends ~10 pointer levels, a sorted vector moves kilobytes per insert; this feed keeps >1000 live levels over ~20,000 ticks | `book-apply` p50 193 → 51 ns; end-to-end -9.5% |
| `std::variant` + `std::visit` for market events instead of a base class | `src/app/pipeline.cpp`, `src/market/market_event.h` | the event set is closed and known at compile time | jump-table dispatch, no vtable on the hot path, and the compiler forces every alternative to be handled |
| Integer tick prices everywhere, one conversion function | `src/common/types.h` | float price comparison is a bug generator | the matcher compares `PriceTick`, so no epsilon logic exists anywhere in the engine |
| `std::stable_sort` for in-flight orders | `src/exchange/matching_engine.cpp` | delivery times collide and submission order has to survive | a replay stays reproducible when two orders are due in the same microsecond |
| Named config structs, designated initializers | `src/app/strategy_factory.h`, `src/strategy/l2_market_maker.h` | the factory took 11 positional arguments, seven of them `double` | a transposed pair of fractions can no longer compile |

### The line reader: blocks, `memchr`, and a stated lifetime contract

```cpp
// src/market/line_reader.h
class LineReader
{
   public:
    static constexpr std::size_t kDefaultBlockSize = std::size_t{1} << 20;  // 1 MiB
    explicit LineReader(const std::string& path, std::size_t block_size = kDefaultBlockSize);
    LineReader(const LineReader&) = delete;                 // owns an ifstream or gzFile
    LineReader& operator=(const LineReader&) = delete;
    bool next_line(std::string_view& line);                 // view into the block: no allocation
    std::uint64_t block_reads() const { return block_reads_; }
};
```

Three things here are deliberate. The header states the contract that makes the `string_view` safe — it is
valid until the **next** call to `next_line()` — and copy construction is deleted rather than shared. A row
crossing a block boundary is compacted to the front of the block, so an owned copy happens only for a line
larger than 1 MiB. And `block_reads()` is exposed so a test can assert that a large file is read in blocks
rather than per line: the counter is part of the interface, not a debugging leftover.

### The book container: a flat array, and the alternative that lost

`TickLadder` stores one side as an array indexed by tick, and its own header carries the argument:

```cpp
/*
 A std::map and a sorted vector both walk a level in a number of dependent steps: a red-black tree
 descends about ten pointer levels, and a sorted vector moves several kilobytes of elements per
 insertion once the book holds a thousand levels. ... Indexing by tick turns a level change into one
 computed address.
*/
```

The sorted `std::vector` was implemented and measured rather than argued away: **1.02 to 1.11x over the
`std::map` in isolation**, which does not justify a kilobyte-scale `memmove` per insertion. That is the point
of the exercise — the rejected alternative is a result, and it also says the two structures pay for the same
shape. Two smaller choices inside the class earned their place the same way: `set()` returns early when the
slot already holds the requested quantity, which an incremental feed does constantly, and `top_levels()`
produces the touch and the summed depth in **one** walk, where asking `nth()` per level restarted from the
best slot and crossed the near-touch slots about five times over.

### Typed events without a vtable

```cpp
// src/app/pipeline.cpp — one event path for every input format
std::visit([this](const auto& value)
{
    using Event = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<Event, MarketTrade>) { ... }
    else if constexpr (std::is_same_v<Event, MarketDepthSnapshot>) { ... }
    ...
}, event);
```

The event set is closed, so `std::variant` costs nothing that a virtual call would not cost more of: dispatch
becomes a jump table, and a new alternative fails to compile until every branch handles it. The pipeline is
still written once for all four input formats.

### Prices are integers, and there is exactly one conversion

```cpp
// src/common/types.h
using PriceTick = int64_t;
inline PriceTick to_price_tick(double price, double tick_size = PRICE_TICK_SIZE)
{
    return static_cast<PriceTick>(std::llround(price / tick_size));
}

// src/exchange/matching_engine.cpp — the matcher compares ticks, never doubles
if (incoming_price < best_price) break;
```

Every price in the engine, the books and the queue counters is `PriceTick`; a `double` price exists only at
the boundary where a feed is parsed and where a report is rendered. That removes the entire class of
"it should have crossed but did not" bugs, because there is no rounding disagreement left to have: the reader
and the engine call the same function.

### Delivery order is a correctness property, not a preference

```cpp
// src/exchange/matching_engine.cpp
void MatchingEngine::advance_to(uint64_t ts)
{
    // The clock has to move even when nothing is in flight, or a cancel latency would never expire.
    now_us_ = std::max(now_us_, ts);
    if (in_flight_orders_.empty()) return;
    std::stable_sort(in_flight_orders_.begin(), in_flight_orders_.end(),
                     [](const auto& left, const auto& right) { return left.first < right.first; });
    ...
}
```

Two orders due in the same microsecond have to land in submission order or the replay stops being
reproducible, which is why the sort is stable rather than fast. The comment on the clock line is the fix for a
real bug: while the engine only moved its clock when something was pending, a cancel latency could never
expire on a quiet market.

### The strategy factory takes a struct, not eleven arguments

```cpp
// src/app/strategy_factory.h
struct StrategyFactoryConfig
{
    const InstrumentSpec* instrument = nullptr;
    FlowAwareMarketMakerConfig flow_config{};
    double l1_risk_threshold = 0.60;          // four of the old arguments were fractions
    double l1_stress_spread_multiplier = 2.5; // of the same type, in the same position
    ...
    uint64_t order_size_override = 0;         // ... and two were counts of the same type
};
std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name,
                                        const StrategyFactoryConfig& config = {});
```

The old signature was eleven positional arguments, seven of them `double`, so at a call site a risk threshold
and a fee multiplier could be swapped and still compile. Named fields with designated initializers make that
class of mistake unwritable, and the C++20 rule that initializers follow declaration order is what makes a
call site readable in review.

## 3. Where the time went

One line per attempt, including the ones that paid nothing. "Effect" is a paired measurement on a fixed
workload; the full table and the derivations are in [Performance](PERFORMANCE.md).

| Change | Effect | Status |
|---|---|---|
| Buffered line reader (1 MiB blocks, `memchr`, `string_view`) | -16.0% end to end; the line layer alone within 10% of the raw scan floor | kept |
| `TickLadder` flat array in the market book | `book-apply` p50 193 → 51 ns; -9.5% end to end | kept |
| Symbol ids keyed by integer in the engine | `engine` p50 141 → 111 ns, 5/5 pairs | kept |
| One bounded top-level walk instead of `nth()` per level | `market-view` p50 95.7 → 68.4 ns; output byte-identical over 11,403,032 batches | kept |
| Integer price and quantity parsing instead of `std::from_chars<double>` | `read+parse` p50 -65.5 ns (-11.3%) | kept |
| Sorted `std::vector` for the book | 1.02-1.11x over the map, in isolation | **rejected** |
| Four inline update slots, column-role jump table, boolean dispatch | no measurable effect (+5.4 ns ± 12.4) | kept, unproven |

`read+parse` is still 61% of the event and the last two attempts to reduce it both measured as no effect,
which is the useful conclusion: the remaining cost is not in the row loop's micro-structure. Cost intuition on
this codebase was wrong by **3 to 10x** in a consistent direction, which is why the ledger is a table of
attempts rather than a list of wins.

## 4. The gate

One script, four tiers, run on every change. `tools/verify_l2.sh all`:

| Tier | Time | What it checks |
|---|---|---|
| 0 | 0.2 s | CTest: 15 unit-test binaries |
| 1 | ~3 s | pinned invariants on an L2 and an L1 run, every number quoted in the documents, and the prose lints |
| 2 | ~15 s | the SHA-256 of the full 1.5 GB standard output against a stored reference |

```bash
# tools/verify_l2.sh — tiers 0 and 1
tier0()  { ctest --test-dir "$build_dir" ...; }                       # a red test cannot pass
tier1()  {
  python3 tools/check_written_fields.py   # every summary field is actually computed
  python3 tools/lint_prose.py docs/*.md   # every flag, queue model and tools/ path exists
  "$BIN" l2_replay ... | grep -oE 'submitted_orders=406|trade_reports=1|...'   # pinned invariants
  python3 tools/check_docs.py --binary "$BIN" docs/PERFORMANCE_HISTORY.md docs/ARCHITECTURE.md docs/DESIGN_REVIEW.md
}
```

The last line is the part worth stealing. A table in a document can carry a marker — the first line below is
written so that it is *not* a marker itself, which matters because the checker matches a line that is exactly
the marker text:

```markdown
<!-- toyquant:check            <- this line and the next two form a marked block
run: l2_replay <the full command, one line>
then: submitted_orders=182 trade_reports=21 fill_rate=0.115385 captured_edge_per_unit_ticks=0.0952381
-->
```

`check_docs.py` re-runs the command and fails the build if any value has moved. A documented number is a
claim about the code, so it is treated as one. `lint_prose.py` covers the other half: it reads the flag
literals out of the parser and fails when a document omits a flag the binary accepts, invents one it does
not, or names a `tools/` script that is missing. Both lints have caught real drift here, twice on flags.

Two habits come out of this design, and both are in the history as failures:

* **The default run is byte-identical.** Every knob defaults to inert, so a feature lands without moving the
  pinned output. When a change *does* alter the default — five new summary fields, once — the reference is
  re-recorded only after proving the difference is exactly those fields, by stripping them from the new
  output and hashing the remainder against the old reference. It matched: the change really was five fields.
* **Tier 0 exists because the gate lied once.** A commit landed carrying a failing assertion, because the
  script compared replay outputs and never ran the unit tests. The fix was to put CTest first, and the first
  change made under the new tier was caught by it.

## 5. What it measures

Five results, one line each. The first four come from the marked block above and its neighbours; the strategy
rows are in [Performance History](PERFORMANCE_HISTORY.md).

| Result | The number |
|---|---|
| **A fill is a queue event, not a price event** | the median quote never came within 3.9% of the front of its queue, and every order that reached the front traded: 21 fills from 21 orders that got there |
| **Adverse selection takes most of the quoted spread** | 0.095 ticks per contract captured out of the 0.5 the quote was placed at |
| **The fee is the binding constraint** | 2 basis points on a 10 USD contract is 2.57 ticks, against 0.095 captured: the policy earns 3.7% of the fee it pays, and 29 times less than it at 267 fills |
| **Latency has two legs with opposite signs** | +20 ms of arrival latency: fills 21 → 11 and captured edge 0.095 → 4.09 ticks. +20 ms of cancel latency: fills → 25 and captured edge → -0.440 ticks |
| **Widening does not create value, it reduces participation** | captured edge per contract rises 100x while total captured value falls; the loss shrinks only because the strategy stops trading (1 fill instead of 267) |

## 6. What it deliberately does not do

* **No connectivity, no FIX, no recovery, no risk gateway, no persistence.** The loop is meant to be readable
  in one sitting, and none of those would make a single number here more trustworthy.
* **No market impact and no feedback from our own resting size** into the queue in front of us.
* **One venue, one instrument, one day.** Deribit BTC-PERPETUAL on 2020-04-01, in windows of 200,000 to
  2,000,000 rows.
* **The queue is a model, not a reconstruction.** The feed carries aggregated quantities, not order
  identities, so the queue in front of a quote is inferred. The mitigation is bounds rather than a single
  calibrated rule: `conservative`, `prorata` (the default), `lumpy` and `optimistic`, plus `--arrival-share`
  for how much of the displayed size a new quote starts behind.
* **Fill counts below thirty are not conclusions.** Several tables here report one to five fills, and every
  one of them says so next to the row rather than in a footnote.
* **The L1 path is the project's first iteration, kept as a regression fixture.** It models no queue in front
  of a quote, so a fill there is a price event; the history of why the mainline moved to L2 is in
  [Performance History](PERFORMANCE_HISTORY.md).

## 7. Interview versions

### 30 seconds

> A C++20 market-making simulator: Deribit depth and trades replayed through a book, a strategy, a matching
> engine with an explicit queue and a portfolio. The two things a normal backtest gets wrong are filling at
> the touch and summarising everything as PnL, so mine fills on queue position and reports the edge per
> contract in ticks next to the fee. The headline: at a one-tick spread the strategy captures 0.095 ticks per
> contract against a fee worth 2.57, so it earns 4% of the fee it pays — and every number in that sentence is
> re-computed by the build.

### The questions

| Question | The answer |
|---|---|
| "Isn't the queue model arbitrary?" | It is a model, and it is bounded: four rules from a conservative lower bound to an optimistic upper bound, plus an arrival share. Measuring the share showed it barely matters, because the pro-rata share is a fixed point of the update rule, and only trades move a level's queue materially (0.04% of quantity changes). So the dynamics decide the queue, not the calibration. |
| "How do you know the numbers are right?" | The build re-runs the command behind every documented figure and fails if a value moved; prose is checked for flags, queue models and tool paths that do not exist; the unit tests run first; and the default run is hashed against a reference, so a faster-but-different change cannot slip through. When the default did change once, I proved the difference was exactly the new fields by stripping them and hashing the rest. |
| "What was the hardest bug?" | A fee model that charged `price * quantity` on an inverse contract, about 630x too much, which had turned an entire strategy comparison into a measurement of the overcharge. It was found by writing down what the fee should be, not by reading the code. The second one is a silent order rejection: a strategy that believed it was quoting had nothing resting, so the position gate leaked and the strategy's bookkeeping drifted. |
| "How do you profile?" | An in-process stage profiler (`StageProfiler`, null-gated so it costs nothing when off) reports per-stage percentiles; changes are accepted on paired repeated runs with a t-statistic, never on a single reading. Absolute numbers do not travel between sessions here — one unchanged binary measured a stage at 581 ns and 834 ns twenty minutes apart — so only paired deltas are quoted as evidence. |
| "Why C++, and why not Python?" | The workload is 1.5 GB of parsing plus a book update per batch, and the parts worth deciding are the container, the allocation behaviour and the memory access pattern. A profiler inside the process is also what made the 3-to-10x-wrong cost intuition visible. |
| "What would you do differently?" | Build the gate before the features: it caught two undocumented flags, a paragraph describing a model set that had changed, and a failing test that shipped in a commit of mine. I would also start from the fee model and the queue model, because everything downstream is a measurement of those two. |
| "What next?" | Parse the depth file in blocks — parsing is 61% of the event and the row loop has already paid nothing twice, so the only shape left is reordering the work. Then put a sanitizer build in the gate, which is the one standard C++ check this project does not run yet. |
