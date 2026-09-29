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
  is. The measured spread on W1 for one unchanged binary is `read+parse` p50 of 581 ns in one session
  and 834 ns twenty minutes later, a 1.4x difference, with every stage's share unchanged.

### 3.2 Paired alternation

`tools/ab_bench.sh` does this. It runs the two builds in ABBA order within a block, takes the paired
difference per block, and reports each stage's mean difference with its t statistic. Three things were
learned by pointing it at a null pair, meaning the same binary on both sides:

- **The first run of a block is slower, and ABBA assigns that position to A**, which biases every
  result toward B. Running one binary as both A and B produced differences of -5% to -13% on every
  stage with `t` up to 3.6 until a discarded warm-up run was added at the start of each block.
- **The floor of the protocol is about 15%, not `t = 2.8`.** Four null comparisons of one binary
  against itself, five blocks each, produced per-arm differences of up to 13% with `|t|` never above
  2.0, and the sign changed between sessions. Randomising which label receives the outer ABBA positions
  did not reduce it, so most of it is noise rather than a position effect. With five blocks the per-arm
  standard error is around 7%, so effects below roughly 15% are not claimable here, whatever their `t`.
- **Two blocks are not enough.** A null pair at two blocks reached +12% on the wall clock and -9% on
  `strategy`.
- **Measure the null in the same session.** Pass `ctrl` and the harness runs an A-versus-A pair
  interleaved with the A-versus-B pair, so that session's artifact is visible next to the effect. An
  effect is only believed when it stands clear of the null measured alongside it.

The practical consequence is a rule about which changes are worth attempting: **prefer changes whose
mechanism predicts an effect larger than the floor, and do not ship a change whose only argument is a
sub-floor measurement.** An isolation benchmark can still justify a mechanism, and a byte-identical
full-file run can still prove behavior, but neither of them is a speed claim.

Stage-level claims are stronger than end-to-end ones for the same reason a paired difference beats two
medians: the stage isolates the code under test from the rest of the event. Every stage is printed,
including the ones that were not supposed to move, because a change confined to one stage that moves
another is a measurement problem rather than a result.

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

- **The numbers depend on the sampling interval.** `read+parse` measures 381-445 at interval 1, 414-551
  at 64 and 435-686 at 512, and the sweep gives the same order in both directions, so it is not drift.
  With a large interval the profiler's own per-stage arrays are cold when a region is timed, and those
  misses are charged to the region. Interval 1 is the cleanest and the slowest to run; the tables in
  section 4 use 64, so their absolute values carry roughly 20% uncertainty from this alone.
- **The stage profiler and gprof are not comparable.** gprof runs on RelWithDebInfo with `-pg` and
  NDEBUG; the stage profiler runs on the Debug build with asserts live, which is what `ctest` needs. In
  one session `read+parse` measured 566 ns under RelWithDebInfo and 651 ns under Debug, while the
  unprofiled wall clock differed by only 3%. Stage figures may be compared within one build and one
  session, never across.
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

### 3.7 The fast loop

Verifying every edit with the full-file comparison would make one iteration cost about a minute, so
the checks are split by what they are for:

| Tier | Command | Cost | Purpose |
|---|---|---|---|
| 1 | `bash tools/verify_l2.sh fast` | ~3 s | L2 200k and L1 invariants plus the numbers quoted in the documents, on every edit |
| 2 | `bash tools/verify_l2.sh all` | ~20 s | adds the full-file stdout hash, before accepting |

Tier 1 compares batch counts, order and fill counts, realised PnL and consumed queue position, which is
what a reader or book change can actually break. Tier 2 is what makes a timing claim defensible,
because byte-identical output over 11.4M batches rules out every explanation except cost. The full-file
reference is kept as a hash, so tier 2 costs one 1.5 GB run instead of two, and `ref` re-records it
when a change is accepted.

### Checking the numbers in the documents

A document that quotes replay output is a claim about the code, and this project's claims are its
product. Hand-copied claims drift: one did, a "thirty times per fill" that a re-measurement turned
into ten. Comparisons are therefore emitted by `tools/compare_runs.sh --markdown`, which prints the
table and a `toyquant:check` block carrying the exact command and the values it produced.

```
python3 tools/check_docs.py docs/PERFORMANCE_HISTORY.md docs/ARCHITECTURE.md
```

That re-runs each recorded command and fails when a quoted number has moved. Only marked blocks are
checked, so prose that records history under conditions which no longer exist is left as it was, and
a table is checked exactly when someone decided it should be.

Two more things keep the loop short:

- **The measured lever matrix is one command.** `bash tools/workload_matrix.sh [placement|requote|model|arrival]`
  runs each lever around the same window and prints the fill counts, the per-contract edge and the fees,
  so the ordering recorded in `PERFORMANCE_HISTORY.md` can be reproduced rather than trusted.

- **Build one target while iterating.** A change to a header such as `tick_ladder.h` is included by
  eight targets, so a plain `cmake --build` rebuilds all of them: 25 steps instead of 2. Use
  `cmake --build out/build/linux-debug --target toy_quant`, then run the whole build plus `ctest` once
  when accepting the change.
- **Let the harness do the pairing.** `bash tools/ab_bench.sh <binaryA> <binaryB> [blocks]` runs ABBA
  blocks and prints the per-stage paired difference. It prints every stage, including the ones that
  were not supposed to move, which is how a measurement problem is told apart from a result. It also
  regenerates the data slices when `/tmp` has been cleared.

### 3.8 Counting what a profile cannot see

A function level profile samples the program's own text, so a call that resolves into a shared library
looks like a leaf with no body: the call appears, its cost does not. `tools/count_libc_calls.c` is an
`LD_PRELOAD` shim that interposes the few calls that matter for a market data reader and reports counts,
byte totals and a size histogram.

```bash
gcc -O2 -shared -fPIC -o /tmp/count_libc_calls.so tools/count_libc_calls.c -ldl
LD_PRELOAD=/tmp/count_libc_calls.so ./out/build/linux-debug/toy_quant l2_replay ... 2>&1 | grep SHIM
```

Counting is cheap and timing each call would not be, since a clock read costs more than some of the
calls. The tool therefore answers "how many, and what shape" and leaves the per-call cost to the
isolation benches. On the 2M slice, per event over 1,303,131 events:

| Call | Per run | Per event | Shape |
|---|---:|---:|---|
| `memchr` | 28,549,312 | 21.9 | 1.5 calls at n~1 MiB are the line scan, ~13.5 at n<=256 are the eight comma searches per row |
| `memcpy` | 11,629,137 | 8.9 | 8.0 of them at n<=16 bytes |
| `read` | 279 | 0.0002 | 156 MB in 1 MiB blocks |

Two things follow. **The requested byte totals must not be read as work**: `memchr` stops at the first
match, so the 1.1 TB this shim printed on its first outing was the sum of the lengths *asked for*, not
the bytes touched, and it nearly sent the search in the wrong direction. And **the calls are too few and
too short to be the cost**: 22 `memchr` and 9 `memcpy` per event is what a 77 byte row with eight fields
should produce, and 279 `read` calls for a 154 MB input is one per megabyte.

With `/usr/bin/time -v` reporting 3.4% system time, zero major page faults and 8 MB peak RSS, this is
what closed the search for a hidden cost in the reader.

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

Rows that moved because of the work in this document: `book-apply` from a p50 of 193 to 51, `engine`
from 167 to 113, `market-view` from 96 to 68, and `read+parse` down a further 11% since this snapshot
was taken. The `read+parse` *share* rose from about 50% to 61% partly because the other stages shrank.

**Read the Share column, not the absolute ones.** This snapshot is one machine state. The host drifts:
the same unchanged binary measured `read+parse` at 581 ns in one session and 834 ns twenty minutes
later, a 1.4x spread, while every stage's share stayed within a percentage point. Absolute values here
describe a session, and only the paired deltas in the ledger travel between them.

## 5. Optimization ledger

One row per attempt, including the ones that did not pay. "Effect" is a paired measurement unless
noted otherwise. The mechanism behind each accepted row is explained in
[Low-Latency Design](LATENCY_DESIGN.md).

| Change | Mechanism | Verified effect | Status |
|---|---|---|---|
| **Buffered line reader** | 1 MiB blocks, `memchr` line boundaries, `string_view` into the block | **-0.258 s (-16.0%)** end-to-end, W1 2M slice, 9 pairs; -10.2% with output on. Line layer alone 0.115 -> 0.080 s (1.44x) | kept |
| **`TickLadder` book container** | one flat array indexed by price tick instead of a red-black tree | `book-apply` p50 **193 -> 51**; in-process book **1085.3 -> 804.3 ns/batch (-25.9%)**; end-to-end -9.5% (200k slice, t=-2.83), -6.0% (2M slice), -6.8% (full file, `%U`) | kept |
| **Symbol id registry in the engine** | per-symbol state keyed by integer instead of by symbol string | `engine` p50 **141 -> 111 (-21.3%)**, 5/5 pairs, t=-2.85; `engine` mean -31.7% | kept |
| **Single-pass top-level walk** | one bounded walk per side produces the touch and the depth sum, instead of `nth(0)`..`nth(4)` restarting from the best slot each time | `market-view` p50 **95.7 -> 68.4 ns (-28.5%)**, 5/5 pairs, t=-25.3; column sum -46.4 ns (-5.7%); end-to-end wall clock -5.9%, t=-2.61; `read+parse` and `book-apply` unchanged as controls; output byte-identical over 11,403,032 batches | kept |
| Reader micro-optimizations | four inline update slots per batch, column-role jump table, `parse_boolean` first-character dispatch | **no measurable effect**: ABBA paired mean +5.4 ns, SE 12.4, over four blocks on a ~530 ns stage | kept, unproven |
| **Fast path for `parse_uint64`** | two digits per step with the validation folded into the loop, for values of at most 18 digits, plus the cold throwing path moved out of the hot function | `read+parse` p50 **-44.8 ns (-9.2%)**, 5/5 pairs, t=-6.2; column sum -67.3 ns (-8.4%); end-to-end wall clock -6.0%, t=-4.49; output byte-identical over 11,403,032 batches | kept |
| Sorted `std::vector` for the book | binary search, plus `memmove` on insert | 1.02-1.11x over the map, in isolation | **rejected** |
| **Integer-plus-scale price parse** | parse the price with integer arithmetic instead of the out-of-line `std::from_chars<double>` in libstdc++ | `read+parse` p50 **-65.5 ns (-11.3%)**, 5/5 pairs, t=-5.9, reproduced at -71 ns in a second session; end-to-end wall clock **-7.6%**, t=-2.83; output byte-identical over 11,403,032 batches | kept |
| Fixed-point price parsing | integer-only price path in the reader | rejected on an isolation comparison (17 ns vs 10 ns) whose in-context cost is about 48 ns per call | **superseded** by the row above |
| Removing the book mutex | drop the lock from `L2OrderBook` | 4.2 ns of a ~1200 ns event (0.7%) | **rejected**, lock kept |

### Resolution

Read the numbers above with the floor in [section 3.2](#32-paired-alternation) in mind: at five blocks
the per-arm standard error is about 7%, so a stage effect below roughly 15% is not claimable on this
host, and the `t` values in the table come from runs taken before the positional bias and the warm-up
were understood. What is certain for every accepted row is the byte-identical full-file comparison and
the mechanism; the magnitudes of the two smallest rows, the price fast path and the `parse_uint64` fast
path, are direction-confirmed but not resolved. The two reader fast paths measured together against the
state without them gave `read+parse` -24% with a same-session null of +1.6%, which is above the floor
but whose staging was contaminated: `engine` and `market-view`, which the change cannot touch, moved
-25% and -19% in the same run.

### Session total

The three changes added in the session that produced the price, `parse_uint64` and `market-view` rows
measure **-16.1% on the column sum and -11.9% on the end-to-end wall clock** (`t = -4.60`, 4/4 pairs)
against the state before them.

Their individual end-to-end estimates were -7.6%, -5.9% and -6.0%, which compound to about -18%. The
measured total is smaller, which is the usual result: each change removes a fraction of what remains,
so the later ones have less left to take. **Individual paired effects are not additive, and only the
cumulative measurement settles the total.**

### What the ledger says

Of nine attempts, six moved a stage by at least 8%, one changed no measurable number at all, and two
were rejected — one of them wrongly, which is now recorded as superseded. Six predictions were made and
five of them were wrong:

| Prediction | Measured |
|---|---|
| Rewriting the price parse without the general purpose double parser is not worth it | **wrong, and it cost the largest remaining single win.** The isolation comparison said 1.7x; the in-context cost is ~48 ns per call, and removing it is -11% of `read+parse` |
| An ablation that removes one call measures that call | **wrong by 4x.** Collapsing the price to two values also shrank the whole run's memory footprint, so its 259 ns was ~65 ns of parse plus ~194 ns of cache effect |
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

1. **The "unattributed half" of `read+parse` does not exist, and finding that closed the question.** An
   earlier reading said gprof accounted for only half of `read+parse` and that the rest sat in invisible
   library code. That comparison was invalid twice over: gprof runs on RelWithDebInfo with `-pg` and
   NDEBUG while the stage profiler runs on the Debug build with asserts live, and the two were measured
   in different sessions. Counting the candidates directly ruled them out, per event over 1,303,131
   events: **279 `read` syscalls per run** (156 MB) with **3.4% system time**, **0 major page faults**
   and 8 MB peak RSS; **22 `memchr`** and **9 `memcpy`** calls, all short, the longest `memchr` being the
   single line scan each row already needs; and removing one `malloc`/`free` pair per batch was measured
   as no effect earlier in the ledger. I/O, syscalls, page faults and hidden library calls are each too
   small to be worth attacking.
   What is left in `read+parse` is our own code, and it is close to its floor: the number conversions
   were the two real levers and both are gone, and the field split measured within 12% of a hand-tuned
   word-at-a-time scan. **The one shape change left is parsing a block in bulk instead of a row at a
   time**, and the question that decides it is whether reordering the work can beat a loop that is no
   longer doing anything obviously wasteful.
2. **`market-view`: done, and the stage is now about 8% of the event.** The single-pass walk landed
   (ledger above). What remains inside the stage is two `price_from_tick` conversions, a `std::string`
   copy of the symbol per view, and the imbalance arithmetic. The 1024-slot growth margin is still an
   untuned knob: it sets how many empty slots a walk crosses, and it has never been measured.
3. **`engine`, 13%.** What remains is not lookups. Each batch copies a `BboQuote` (two SSO strings)
   into `latest_quotes_`, `l2_top_bbo_` and `external_bbo_`, and `MEOrderBook` still uses a
   `std::map` per price level. The book-container result in the ledger is the argument for measuring
   a `TickLadder` there too, but order flow is 406 orders per 117,288 batches on W1, so a workload
   with real order flow is needed before that can show anything.
4. **`validate` (5%) and `strategy` (4%)** stay out of scope until the three above are settled.





