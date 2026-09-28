# Performance

This is the reference for what the engine costs and how those numbers were obtained. It has four
parts: the workloads, the measurement method, the current stage breakdown, and the ledger of every
optimization that was attempted.

Three rules keep it honest:

- **A number lives in one place.** Reasoning about *why* a design is fast is in
  [Low-Latency Design](LATENCY_DESIGN.md). Measurements that have been superseded are in
  [Performance History](PERFORMANCE_HISTORY.md). Nothing is restated.
- **An effect is claimed as a range from paired runs, or not at all.** This host varies by about
  +-22% between runs, so an unpaired comparison smaller than 5% means nothing. Several ledger rows
  below read "no measurable effect", and that is a result, not a failure to report.
- **Behavior is pinned before speed is claimed.** Every change below was accepted only after the
  invariant list in [section 3.4](#34-the-invariants-that-must-not-move) came back byte-identical.

## 1. Workloads

Four inputs are used. W1 is the latency target; the others exist to catch behavioral regressions.

### W1: L2 incremental replay (primary)

Deribit BTC-PERPETUAL, one day of trades merged with an incremental depth stream. This is the
workload every latency number below refers to.

```bash
./out/build/linux-debug/toy_quant l2_replay \
  data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  data/v2/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv \
  BTC-PERPETUAL 0 active_l2 1 conservative --no-output --fast-validation
```

The full depth file is 1.5 GB and 11.4M batches, which is slow to iterate on, so two slices of the
same file are used for day-to-day work:

| Slice | Rows | Batches | Events | Use |
|---|---:|---:|---:|---|
| `/tmp/depth_200k.csv` | 200,000 | 117,288 | 236,524 | quick correctness check |
| `/tmp/depth_2m.csv` | 2,000,000 | 1,183,894 | 1,303,131 | stage timings |

Both are `head -n` prefixes of the full file, so they are the same data at different lengths, not
different data. The 200k slice is small enough that everything stays in page cache.

The shape of this feed is the reason the book container dominates the design discussion: **1.71
level updates per batch**, more than a thousand live levels per side, and a live price span of about
20,000 ticks.

### W2: Binance L1 replay (regression)

```bash
./out/build/linux-debug/toy_quant replay \
  data/v2/test_aggTrades_5k.csv data/v2/test_bookTicker_5k.csv BTCUSDT 0 optimized 1000000
```

### W3: Legacy CSV plus backtest (regression)

```bash
./out/build/linux-debug/toy_quant csv data/scenarios/flat_ticks.csv 0 optimized
./out/build/linux-debug/backtest_main data/scenarios/flat_ticks.csv \
  data/runtime/orders.csv data/runtime/trades.csv 0 0 backtest logs/backtest.log
```

### W4: strategy benchmark (regression)

```bash
/usr/bin/time -f '%e' ./out/build/linux-debug/strategy_benchmark \
  data/v2/test_aggTrades_5k.csv data/v2/test_bookTicker_5k.csv BTCUSDT 1000000
```

## 2. Environment

- Linux x86_64 under WSL2, Intel Core Ultra 7 258V, 8 online CPUs.
- CMake preset `linux-debug`, which is `CMAKE_BUILD_TYPE=Debug` plus the project's `-O2
  -march=native`, and Debug adds `-g`.
- `/usr/bin/time` is available. `perf` and `valgrind` are not. `gprof` needs the separate
  `linux-relwithdebinfo` preset, and `StageProfiler` replaced it for routine work.
- WSL2 puts the page cache and the CPU governor outside the repository's control. Treat every
  absolute number as a property of today's machine, and every paired difference as a property of the
  code.

## 3. How to measure on this host

### 3.1 The noise floor

Repeated runs of an unchanged binary on W1 spread over roughly +-22%. On the 1.5 GB workload the
spread is larger still, because the wall clock then tracks whether the file is in page cache. Two
consequences follow, and both are load-bearing:

- **A single run cannot resolve anything below about 5%.** Any claim in this document that rests on
  one run is labeled as such.
- **Absolute numbers are not comparable across sessions.** Only a ratio measured in the same session
  is.

### 3.2 Paired alternation

The only method used here is to alternate the two builds and compare per pair:

```bash
for i in 1 2 3 4 5; do
  before=$( { /usr/bin/time -f '%e' ./tq_before ...args... > /dev/null; } 2>&1 | tail -1 )
  after=$(  { /usr/bin/time -f '%e' ./tq_after  ...args... > /dev/null; } 2>&1 | tail -1 )
  echo "$before $after"
done
```

Thermal and scheduler drift then move both builds together and cancel in the difference. When the
drift is monotonic across the run, use ABBA ordering (before, after, after, before) and compare block
means, which cancels a linear drift as well. Stage-level claims are stronger than end-to-end ones for
the same reason a paired difference is stronger than two medians: the stage isolates the code under
test from the other 90% of the event.

Building the pair for a comparison, when the change under test is one or two files:

```bash
cp out/build/linux-debug/toy_quant /tmp/tq_after
git stash push -- path/to/changed/files
cmake --build out/build/linux-debug --target toy_quant
cp out/build/linux-debug/toy_quant /tmp/tq_before
git stash pop
cmake --build out/build/linux-debug --target toy_quant
cmp out/build/linux-debug/toy_quant /tmp/tq_after   # the rebuild must reproduce the after binary
```

The last `cmp` matters: it proves the pair differs only in the stashed files. Piping a compiler's
output into `head` can raise SIGPIPE and leave a stale binary in place, which silently invalidates a
measurement by making both builds identical.

### 3.3 Wall clock, user time, and I/O

On W1 the wall clock is dominated by reading 1.5 GB, so a CPU change disappears into page-cache
noise. `%U` from `/usr/bin/time` isolates the CPU. The difference between the two is why the
full-file rows of the ledger look smaller than the slice rows: the same change, measured with the
I/O still in the figure.

### 3.4 The invariants that must not move

Every change is verified against these before any timing is taken. They cover all four workloads, so
a change to the book container cannot silently alter the L1 path, and vice versa.

| Workload | Invariant |
|---|---|
| W2 (L1) | `submitted_orders=13734 trade_reports=2277 fill_rate=0.175872 realized_pnl=-33.7861 equity=966.196` |
| W1, 200k slice | `incremental_batches=117288 submitted_orders=406 trade_reports=1 queue_ahead_consumed=55460 realized_pnl=-0.002 equity=1000` |
| W1, full file | `incremental_batches=11403032 submitted_orders=26145 trade_reports=222 realized_pnl=-1.0615 equity=998.939` |
| W3 | `equity=1000.13 max_drawdown=2.49569e-05` |
| All | `ctest` green, zero new compiler warnings |

The strongest check available for a change that touches the book or the engine is to diff the whole
stdout of a W1 run against a stored copy:

```bash
./out/build/linux-debug/toy_quant l2_replay ...args... > /tmp/full_after.txt 2>&1
diff -q /tmp/full_before.txt /tmp/full_after.txt
```

Byte-identical output over 11,403,032 batches is what "behavior preserving" means here, and it says
more than any unit test can.

### 3.5 Stage timing

`--profile-stages` turns on `StageProfiler`, which times eight regions per event on every 64th event.
The profiler reads the clock twice at construction and subtracts the minimum of those reads from
every sample (35-50 ns here), because otherwise each region would be charged for a clock read and the
column sum would come out larger than the wall clock.

```bash
./out/build/linux-debug/toy_quant l2_replay ...args... --no-output --fast-validation --profile-stages
```

Reading the output:

- **Use the `p50` column, and use share as a ranking.** The mean is unusable for stages with rare
  stalls: `read+parse` reaches tens of microseconds at p999 because one sample can absorb a page
  fault or a 1 MiB block refill. Those samples move the mean by hundreds of nanoseconds and leave the
  median where it belongs.
- The p50 column sum lands within 10-15% of the per-event wall clock, so the eight regions cover
  essentially the whole event. Treat the sum as a ranking tool and the wall clock as the absolute
  figure.
- Repeating a stage measurement three times and taking the median of the p50 values costs seconds and
  removes most of the run-to-run spread. Section 4 does exactly that.

### 3.6 Why isolation benchmarks mislead

Standalone microbenchmarks exist for the line layer, the field split, the number conversion, the
container choice, the mutex, and the string hash. They are useful for one thing: answering a
*mechanism* question. The string hash bench is the clean example. It measured

```
string  key: find(s) then [s] = v   23.8 ns
string  key: find(s), reuse iterator 13.7 ns
integer key: find(k) then [k] = v   10.5 ns
```

and the 10.1 ns between the first two lines is one hash, which settles a question the source could
not: `operator[]` re-hashes the key it was just given rather than reusing the hash from `find`. That
is a mechanism answer, and it is the only kind of answer this bench can give.

It is misleading for *priority* questions, and this project has the receipts:

- A line-layer benchmark pointed at field splitting as the next target, because that was all it could
  see. Stage timing later showed the field split is a small part of the parser's cost.
- A container benchmark said a book lookup would be 3.9x faster. The stage did improve by 70%, and
  the end-to-end effect was 6-10%.
- An allocation-based explanation ("161 ns per batch must be `malloc`") was wrong: construction and
  moves dominated it.

The rule that came out of this: **microbenchmarks nominate the next candidate, stage timing and
paired end-to-end runs decide whether it ships.**

## 4. Current stage breakdown

Median of three `--profile-stages` runs on the 2,000,000-row slice: 20,361 samples over 1,303,131
events, with every change in this document applied.

| Stage | Share (p50) | p50 | p90 | p99 | p999 |
|---|---:|---:|---:|---:|---:|
| read+parse | **61.1%** | 528 | 654 | 897 | 21840 |
| validate | 5.3% | 46 | 81 | 145 | 340 |
| book-apply | 5.9% | 51 | 100 | 166 | 414 |
| market-view | 11.1% | 96 | 153 | 230 | 526 |
| top-booking | 1.3% | 11 | 53 | 96 | 215 |
| engine | **13.1%** | 113 | 159 | 275 | 1647 |
| queue-acct | 0.5% | 4 | 25 | 54 | 141 |
| strategy | 4.2% | 36 | 89 | 157 | 2935 |
| **column sum** | 100% | **864** | | | |

Two rows moved because of the work in this document, and both moved far outside the noise:
`book-apply` from a p50 of 193 to 51, and `engine` from 167 to 113. Those are the two rows the ledger
below is responsible for. The `read+parse` share rose from about 50% to 61% purely because the other
stages shrank; its own p50 did not improve.

## 5. Optimization ledger

One row per attempt, including the ones that did not pay. "Effect" is a paired measurement unless
noted otherwise. The mechanism behind each accepted row is explained in
[Low-Latency Design](LATENCY_DESIGN.md).

| Change | Mechanism | Verified effect | Status |
|---|---|---|---|
| **Buffered line reader** | 1 MiB blocks, `memchr` line boundaries, `string_view` into the block | **-0.258 s (-16.0%)** end-to-end, W1 2M slice, 9 pairs; -10.2% with output on. Line layer alone 0.115 -> 0.080 s (1.44x) | kept |
| **`TickLadder` book container** | one flat array indexed by price tick instead of a red-black tree | `book-apply` p50 **193 -> 51**; in-process book **1085.3 -> 804.3 ns/batch (-25.9%)**; end-to-end -9.5% (200k slice, t=-2.83), -6.0% (2M slice), -6.8% (full file, `%U`) | kept |
| **Symbol id registry in the engine** | per-symbol state keyed by integer instead of by symbol string | `engine` p50 **141 -> 111 (-21.3%)**, 5/5 pairs, t=-2.85; `engine` mean -31.7% | kept |
| Reader micro-optimizations | four inline update slots per batch, column-role jump table, `parse_boolean` first-character dispatch | **no measurable effect**: ABBA paired mean +5.4 ns, SE 12.4, over four blocks on a ~530 ns stage | kept, unproven |
| Sorted `std::vector` for the book | binary search, plus `memmove` on insert | 1.02-1.11x over the map, in isolation | **rejected** |
| Fixed-point price parsing | integer-only price path in the reader | no gain over `std::from_chars<double>` in the same loop | **rejected** |
| Removing the book mutex | drop the lock from `L2OrderBook` | 4.2 ns of a ~1200 ns event (0.7%) | **rejected**, lock kept |

### What the ledger says

Of seven attempts, three moved a stage by more than 20%, two changed no measurable number at all, and
two were rejected. Both rejected ones were *predicted* to be wins before they were measured, as was a
fourth that was only ever measured in isolation:

| Prediction | Measured |
|---|---|
| Rewriting the price parse as fixed point would be a large win | no gain at all |
| A sorted vector beats a tree slightly, so the tree's pointer chasing is the cost | 1.02-1.11x: both structures pay for the same shape |
| The mutex on the book is worth removing | 4.2 ns, 0.7% |
| "161 ns per batch" is allocation cost | it was construction and moves |
| Ten string lookups on the engine hot path should cost ~55 ns | ~30 ns: the isolation bench measures a dependency chain, and the real loop overlaps the hash with other work |

Cost intuition on this codebase is systematically off by 3-10x, always in the same direction. That is
the most reusable finding in this document, and it is why the ledger exists as a table of attempts
rather than a list of wins.

## 6. Where the remaining time is

`read+parse` is 61% of the event, and the last two attempts to reduce it both measured as no effect.
That is the useful conclusion: the remaining cost is **not** in the micro-structure of the row loop,
and further tuning there has now been tried twice and paid nothing.

1. **Change the shape of the parse, not its instructions.** Two consecutive no-effect results on a
   528 ns stage say the loop is not instruction-bound. The untried candidates are structural: parse a
   block in bulk instead of a row at a time, or keep prices as integers end to end so that the
   `double` conversion leaves the hot path instead of being made cheaper inside it.
2. **`market-view`, 11%.** `L2OrderBook::market_view` walks the ladder from the best slot once per
   level (`nth(0)`..`nth(4)`, about 28 slots per batch for five levels), so the near-touch slots are
   walked five times to produce one view. A single bounded walk that fills all five levels is the
   obvious fix, and the 1024-slot growth margin is a tunable that sets how many empty slots a walk
   crosses and has never been tuned.
3. **`engine`, 13%.** What remains is not lookups. Each batch copies a `BboQuote` (two SSO strings)
   into `latest_quotes_`, `l2_top_bbo_` and `external_bbo_`, and `MEOrderBook` still uses a
   `std::map` per price level. The book-container result in the ledger is the argument for measuring
   a `TickLadder` there too, but order flow is 406 orders per 117,288 batches on W1, so a workload
   with real order flow is needed before that can show anything.
4. **`validate` (5%) and `strategy` (4%)** stay out of scope until the three above are settled.





