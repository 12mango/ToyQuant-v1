# Performance History

This is the chronological lab notebook of the performance work: what was measured, in what order, and
what it said at the time. Most of it has been **superseded**, and that is the point of keeping it
separate.

Where things live now:

| Question | Document |
|---|---|
| What does the engine cost today, and how is that measured? | [Performance](PERFORMANCE.md) |
| Why is a given design faster, and what was rejected? | [Low-Latency Design](LATENCY_DESIGN.md) |
| What was measured earlier, and what did it say then? | this document |

Read a number here only as history. The entries that still hold are repeated in the
[optimization ledger](PERFORMANCE.md#5-optimization-ledger); anything not in that ledger was either
superseded by a later measurement or was never a latency claim in the first place.

The order below is the order the work happened. It starts with a reproducible end-to-end baseline
taken before any latency-focused change, which was a regression reference rather than a production
throughput or latency claim.

## Workload

- Executable: `out/build/linux-debug/strategy_benchmark`
- Trades: `data/v2/test_aggTrades_5k.csv` (3,412 data rows)
- BBO: `data/v2/test_bookTicker_5k.csv` (35,310 data rows)
- Symbol and quantity scale: `BTCUSDT`, `1000000`
- Each invocation replays the inputs sequentially for five L1 strategy variants and writes intermediate order/trade files under `/tmp`.
- Trades SHA-256: `f3dc0bcc056dbe27ddfdae330d9e9fa6ebbacd787a3d71692a898ba762450ca3`
- BBO SHA-256: `0b795bd584c16c3a3049690bf4149e5b8aadddbc1f6dc3f6680a295b75a35b1e`

## Environment

- Linux x86_64 under WSL2
- CPU: Intel Core Ultra 7 258V, 8 online CPUs
- CMake preset/build directory: `linux-debug`
- `CMAKE_BUILD_TYPE=Debug`; project compiler options also include `-O2 -march=native` and Debug adds `-g`.
- `/usr/bin/time` is available. `perf` and `valgrind` are unavailable. `gprof` is installed, but the current binary is not built with `-pg`.

## Measurement

The executable was run seven times with the same inputs. Wall-clock seconds, including process startup, input parsing, all five replays, and temporary output writing:

| Run | Seconds |
|---:|---:|
| 1 | 0.20 |
| 2 | 0.14 |
| 3 | 0.15 |
| 4 | 0.16 |
| 5 | 0.16 |
| 6 | 0.16 |
| 7 | 0.16 |

Median: `0.16 s`; range: `0.14–0.20 s`. Captured stdout/stderr hashes matched across all seven runs, indicating deterministic benchmark output for this workload.

Reproduce from the repository root:

```bash
/usr/bin/time -f '%e' ./out/build/linux-debug/strategy_benchmark \
  data/v2/test_aggTrades_5k.csv \
  data/v2/test_bookTicker_5k.csv BTCUSDT 1000000
```

## L2 Mainline Workload

To cover the current mainline, `active_l2` was replayed against a 15-minute incremental Deribit
window with the conservative queue model and no depth sampling:

- Window: `[1585699200000000, 1585700100000000]`
- Strategy / queue model: `active_l2` / `conservative`
- Trades: 1,354 rows; sliced input SHA-256 `af5475b5d26329563baab29519b02f440f700f74f13bd822cbcbd682605a6964`
- Incremental depth: 201,130 rows; sliced input SHA-256 `716a84d48138648ccfd904cadfd434b6822070d4675fed88b5ca3545b200ff29`
- Runtime summary: 119,364 events, including 118,010 incremental batches; one batch is a snapshot/reset.

Five runs took `0.39`, `0.35`, `0.37`, `0.35`, and `0.33` seconds (median `0.35 s`, range
`0.33–0.39 s`). Captured output hashes matched across all five runs:
`afddc9b1741da4ac3000ed476ce406b3ad66d7bcf2d0b05a2cd19d9ccdfcccc3`.

The first parser optimization replaced the per-row `std::istringstream` splitter with an equivalent
delimiter scan and added allocation-free fast paths for common boolean fields. On the same
15-minute input, three post-change runs took `0.20`, `0.22`, and `0.17` seconds (median `0.20 s`),
with the same execution summary and identical output hashes. This is an initial comparison against
the five-run pre-change median, not a final speedup claim; repeat the one-hour workload after the
next change for the official comparison.

Incremental batches now reserve two update slots before appending rows. In the measured window,
117,945 of 118,010 batches contained one or two updates, so this avoids common small-vector
growth without reserving eight slots for nearly every batch. The reserve-size comparison was noisy
at this short duration; the choice is based on the observed distribution and memory behavior, not
on a claimed standalone speedup.

The incremental reader also reuses the CSV field vector while collecting rows in one batch. Three
follow-up runs took `0.20`, `0.20`, and `0.18` seconds with identical output hashes. The difference
from the preceding `0.20`, `0.21`, and `0.20` second sample is within short-run noise; the main
benefit is reducing repeated vector allocation on the hot path.

The integer timestamp, sequence, and amount conversions in the adapter now use `std::from_chars`
through a checked `parse_uint64` helper instead of `std::stoull`. The focused adapter and replay
tests passed, and three fixed-window runs took `0.18`, `0.19`, and `0.18` seconds with identical
outputs. Floating-point conversion remains unchanged because it has different parsing and error
handling considerations.

The Deribit snapshot reader now uses the same `string_view` field path while constructing its typed
25-level snapshot vectors, while retaining explicit vector capacity reservation. Snapshot,
incremental, and orderbook tests passed; three same-window runs were `0.40`, `0.39`, `0.41` seconds
for snapshot input and `0.16`, `0.16`, `0.16` seconds for incremental input, with identical outputs
within each source. The snapshot short-run timing is neutral, but per-row string materialization is
removed from the snapshot parser.

Snapshot map construction now uses insertion hints because the validated input levels arrive in
the exact bid-descending/ask-ascending order required by the maps. L2 tests passed; on a same-window
short replay, snapshot runs took `0.12`, `0.12`, and `0.12` seconds while incremental runs took
`0.06`, `0.05`, and `0.05` seconds, with identical outputs. This optimization is specific to
ordered snapshot construction and does not change incremental update behavior.

The incremental reader now reuses its `string_view` field vector across batches and carries the
already-parsed batch/local timestamps into `append_update`, avoiding repeated integer conversions
for the same row. Three fixed-window runs took `0.14`, `0.14`, and `0.14` seconds with identical
outputs. This targets the long-profile hotspots `split_csv_views`, `parse_uint64`, and
`append_update` as one parser chain.

The L2OrderBook bid/ask maps now use an `unsynchronized_pool_resource` for map-node allocation,
including snapshot temporary maps and reset release. The ordered-map semantics remain unchanged;
this only changes node allocation/reuse. L2 tests passed, and the same short window measured
`0.05`, `0.06`, `0.05` seconds for incremental input and `0.13`, `0.12`, `0.13` seconds for
snapshot input, with identical outputs. Longer-window memory behavior still needs monitoring before
claiming the allocator change as final.

The one-hour no-output memory check reported maximum RSS of `5,464 KB` for incremental input and
`5,172 KB` for snapshot input. Wall times were approximately `0.87 s` and `2.40 s`, respectively;
these runs showed no abnormal pool growth for the measured workload.

The parser chain is now direct for incremental rows: a `ParsedRow` is filled by one column scan and
then converted to `IncrementalBookUpdate`, without constructing a per-row field vector at all. The
same row object also supplies batch grouping and typed conversion values. Adapter/replay tests
passed; three fixed-window runs took `0.15`, `0.15`, and `0.16` seconds with identical outputs.

The same checked `std::from_chars` approach is now used for ordinary floating-point fields, with
`std::stod` retained as a fallback for formats the fast parser rejects. Adapter/replay tests and
three fixed-window runs passed with identical outputs; the short runs were `0.21`, `0.21`, and
`0.21` seconds, so no standalone speedup is claimed yet.

The incremental reader now passes its known column count into the reusable CSV splitter, avoiding
an extra comma-counting scan on that path. Three fixed-window runs took `0.17`, `0.16`, and `0.15`
seconds with identical outputs. This result is still a short-run comparison, but the change keeps
the optimization local to incremental parsing and does not alter CSV field semantics.

Incremental batches now carry optional snapshot metadata computed while the reader appends updates.
The reader, validator, and `L2OrderBook` reuse that flag instead of rescanning every update list;
manually constructed batches without metadata retain the original scan fallback. The focused
tests passed and three fixed-window outputs remained identical (`0.16`, `0.24`, `0.16` seconds),
so the short timing is treated as noisy while the scan reduction is retained as the structural
benefit.

`IncrementalBookUpdate` no longer stores a duplicate `exchange` string: the exchange is already
owned by `IncrementalBookBatch`, and no downstream code consumed the per-update copy. The adapter,
orderbook, validator, and replay tests passed; three fixed-window runs took `0.15`, `0.14`, and
`0.15` seconds with identical outputs. This reduces the per-update object size and string-copy
work without changing the batch-level exchange metadata.

The L2 update path now uses `insert_or_assign` for non-zero price-level quantities, and the snapshot
reader reserves its known depth vector sizes before appending levels. The related tests passed. On
the same 15-minute window, three incremental runs took `0.14`, `0.15`, and `0.15` seconds; three
snapshot runs took `0.34`, `0.36`, and `0.36` seconds. Outputs were identical within each source.

The L2OrderBook price maps now use integer `PriceTick` keys configured with the instrument tick
size, converting to/from double only at the input/output boundary. This removes floating-point
ordering from the core map and makes price-level identity explicit. L2/orderbook/strategy tests
passed; the updated three-run sample was `0.15`, `0.15`, `0.16` seconds for incremental input and
`0.40`, `0.39`, `0.40` seconds for snapshot input. Incremental output remained identical; the
snapshot timing is slightly higher in this short sample, so this change is treated primarily as
a correctness/data-structure improvement, not a claimed universal speedup.

`MarketDataValidator::validate` now performs merged timestamp ordering, event counting, and
variant-specific validation in one `std::visit` instead of dispatching the same `MarketEvent`
twice. Validator/replay tests passed; three incremental runs took `0.16`, `0.16`, and `0.15`
seconds with identical outputs.

The L2 Pipeline now uses `insert_or_assign` for the latest quote map and order lifecycle maps,
avoiding default construction before assignment. Pipeline/accounting, replay, and benchmark tests
passed; three incremental runs took `0.14`, `0.14`, and `0.13` seconds with identical outputs.

`L2ReplayFeed::run` now caches the current trade and depth event timestamps while merging the two
streams, updating each cache only when its reader produces a new event. This avoids repeated
variant visits in the merge comparison. Replay/pipeline tests passed; three incremental runs took
`0.14`, `0.13`, and `0.13` seconds with identical outputs.

The incremental validator now keeps references to the per-symbol stream state and snapshot-active
state during one batch, avoiding repeated unordered-map lookups. Validator/replay tests passed;
the short timing sample was noisy (`0.41`, `0.35`, `0.46` seconds) but outputs remained identical,
so this is retained as a structural cleanup rather than a standalone speedup claim.

Validator stream names are now assembled only when an ordering error is thrown: normal events pass
a static stream type and symbol separately. Quote state also uses `insert_or_assign`. Validator and
replay tests passed; three fixed-window runs took `0.16`, `0.17`, and `0.17` seconds with identical
outputs.

The incremental path now parses rows through `string_view` fields into typed
`IncrementalBookUpdate` values instead of materializing a `vector<string>` for every row. The
per-update symbol was removed because symbol ownership belongs to the batch; the reader validates
the row symbol at construction time. `MarketDataValidationConfig` now exposes
`validate_incremental_update_fields`, defaulting to strict validation while allowing a replay
caller to skip duplicate per-update checks after the reader boundary has been trusted. Adapter,
L2, and pipeline tests passed; three fixed-window runs took `0.18`, `0.15`, and `0.17` seconds with
identical outputs.

Prepare the same slices and replay with:

```bash
profile_dir=$(mktemp -d)
.venv/bin/python - "$profile_dir" <<'PY'
import sys
from pathlib import Path

sys.path.insert(0, "tools")
from benchmark_l2_strategies import slice_csv

root = Path(sys.argv[1])
start_ts = 1585699200000000
end_ts = 1585700100000000
slice_csv(Path("data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz"),
          root / "trades.csv", start_ts, end_ts)
slice_csv(Path("data/v2/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv"),
          root / "depth.csv", start_ts, end_ts)
PY
/usr/bin/time -f '%e' ./out/build/linux-debug/toy_quant l2_replay \
  "$profile_dir/trades.csv" "$profile_dir/depth.csv" BTC-PERPETUAL \
  0 active_l2 1 conservative
```

`l2_replay` writes to the fixed `data/runtime/orders.csv`, `data/runtime/trades.csv`, and
`logs/toy_quant.log` paths. Preserve these files before replay if their current contents matter.

For a compute-focused run that does not touch those files or emit Logger output, append
`--no-output` after the queue model:

```bash
./out/build/linux-debug/toy_quant l2_replay \
  "$profile_dir/trades.csv" "$profile_dir/depth.csv" BTC-PERPETUAL \
  0 active_l2 1 conservative --no-output
```

This mode routes order/trade streams and logs to null sinks. The default file-producing behavior
is unchanged.

For repeatable timing, use the repository runner instead of assembling shell loops manually:

```bash
.venv/bin/python tools/measure_l2_baseline.py \
  --binary out/build/linux-debug/toy_quant \
  --trades data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  --depth data/v2/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv \
  --symbol BTC-PERPETUAL \
  --start-ts 1585699200000000 --end-ts 1585702800000000 \
  --runs 5
```

The runner slices inputs into a temporary directory, defaults to `--no-output`, reports timing
statistics, and verifies that all captured stdout/stderr hashes match. Pass `--fast-validation` to
measure the validation-free comparison path, or `--with-output` only when output I/O is part of the
measurement.

Append `--fast-validation` to skip duplicate per-update incremental field checks after the reader
boundary, while retaining stream ordering and batch validation:

```bash
./out/build/linux-debug/toy_quant l2_replay \
  "$profile_dir/trades.csv" "$profile_dir/depth.csv" BTC-PERPETUAL \
  0 active_l2 1 conservative --no-output --fast-validation
```

On a short fixed window, strict and fast validation both measured `0.04–0.05 s` with identical
empty output streams. This separates the validation cost for future profiling; it is not a claim
that validation is currently the dominant bottleneck.

## gprof Profile Preparation

The opt-in CMake target `toy_quant_profile` builds the same L2 executable with `-pg` and is
excluded from ordinary builds. Build it with the CMake Tools target `toy_quant_profile`, then
reuse the prepared slices above and aggregate repeated runs:

For non-instrumented profiling with optimization and symbols, use the separate
`linux-relwithdebinfo` CMake preset. It writes to `out/build/linux-relwithdebinfo` and does not
alter the existing Debug build directory.

```bash
profile_prefix="$profile_dir/gmon"
for i in {1..50}; do
  GMON_OUT_PREFIX="$profile_prefix" ./out/build/linux-debug/toy_quant_profile l2_replay \
    "$profile_dir/trades.csv" "$profile_dir/depth.csv" BTC-PERPETUAL \
    0 active_l2 1 conservative > /dev/null 2>&1
done
gprof ./out/build/linux-debug/toy_quant_profile "$profile_prefix".* \
  > "$profile_dir/gprof.txt"
```

This 50-run aggregate collected about `4.07` CPU seconds of gprof samples. The leading application
functions were:

| Function | Sample share |
|---|---:|
| `L2OrderBook::apply_incremental_batch` | 11.3% |
| Incremental CSV split/group helpers | 8–10% each |
| `L2OrderBook::market_view` | 6.8% |
| `ActiveL2MarketMaker::on_l2_market_view` | 3.5% |

`_init` accounted for about 19.8%, largely reflecting that this aggregate starts a fresh process
for every run. Treat these percentages as hotspot candidates, not precise event-loop costs: gprof
adds instrumentation overhead, the sample is one short market window, and I/O/process startup are
included. The normal non-instrumented replay timing above remains the performance comparison
baseline.

## Paired One-Hour Baseline

For a larger workload, the same one-hour window was replayed three times per depth source with
`active_l2`, `conservative`, and no depth sampling:

- Window: `[1585699200000000, 1585702800000000]`
- Trades: 9,310 rows; SHA-256 `c678297c64974a6bab3fdd240e3dcb3e75fe32ec401a106d6fa926955320a45d`
- Incremental: 1,113,638 rows, grouped into 669,482 batches plus 9,310 trades; SHA-256 `a0732c30303b3eb2083775edb0c8fb51a5cb8062133fe60af67382d3e8bae4a8`
- Snapshot: 351,516 rows / 360,826 replay events; SHA-256 `28fca5d55c734cac633e18d2980654a25cd6741c82b44b5f3c622356e952c9d4`

| Depth source | Run times (seconds) | Median | Output consistency |
|---|---|---:|---|
| Incremental | 1.90, 1.77, 1.77 | 1.77 | All three captured outputs identical |
| Snapshot | 4.35, 4.10, 3.70 | 4.10 | All three captured outputs identical |

These are separate replay baselines, not expected to produce identical strategy metrics: snapshot
and incremental feeds have different event granularity and state-update semantics.

The 12-run gprof aggregate on the one-hour incremental input retained the same broad ranking as
the short-window profile:

| Function group | Sample share |
|---|---:|
| `L2OrderBook::apply_incremental_batch` | 15.2% |
| Incremental reader append/split/group functions | about 26% combined |
| `L2OrderBook::market_view` | 4.7% |
| `ActiveL2MarketMaker::on_l2_market_view` | 1.6% |
| `_init` and process startup | 18.8% |

The higher-resolution run strengthens the case that input parsing/grouping and L2 state
reconstruction deserve attention before strategy-specific work. The startup share remains a
profiling artifact of launching a new process for every sample.

The unified baseline runner was then used for the final three-run, one-hour no-output comparison:

| Source | Rows | Seconds | Median | Output hash |
|---|---:|---|---:|---|
| Incremental | 1,113,638 | 0.894515, 0.875006, 0.839258 | 0.875006 | identical |
| Snapshot | 351,516 | 2.311291, 2.163872, 2.144108 | 2.163872 | identical |

These numbers are the current compute-only baseline for future L2OrderBook changes. They exclude
runtime CSV/log output and use the strict default validation mode.

## L2 Strategy Regression Matrix

All four retained L2 strategies are replayed on the same 15-minute window for each depth source,
with the conservative queue model. Window `[1585699200000000, 1585700100000000]`: 1,354 trade rows,
201,130 incremental depth rows, 54,170 snapshot depth rows, `quantity_scale=1`. Net PnL is
`equity - 1000`, i.e. the change in equity from the initial cash.

| Strategy | Incremental orders / fills / net PnL | Snapshot orders / fills / net PnL |
|---|---:|---:|
| `passive_l2` | 657 / 2 / -0.020 | 641 / 2 / -0.020 |
| `inventory_aware_l2` | 419 / 0 / 0.000 | 407 / 2 / -0.007 |
| `flow_aware_l2` | 417 / 0 / 0.000 | 363 / 1 / -0.033 |
| `active_l2` | 414 / 0 / 0.000 | 396 / 0 / 0.000 |

**These values supersede an earlier table that reported net PnL between -13 and -21.** That table was
produced while the fee model charged `price * quantity` on a Deribit contract, which is about 630
times the contract's 10 USD face value. Those figures were dominated by the overcharge rather than
by trading, so they said nothing about the strategies. `ARCHITECTURE.md` documents the corrected
model.

### Why the fill counts are near zero

Fees do not influence matching, so the corrected fee model leaves fills unchanged. Two other facts
explain the counts, and both are model behaviour rather than defects:

- The strategies cancel nearly everything they submit. In this window `passive_l2` submits 657
  orders and cancels 643 of them, while `active_l2` submits 414 and cancels 412. Cancelling and
  re-quoting at the same price loses the queue position and rejoins at the back, so the order never
  reaches the front.
- Under the default `conservative` queue model the queue advances only through real trades. A real
  queue at the touch mostly drains by cancellation, so requiring the whole displayed size to trade
  at your exact price makes a fill close to impossible.

The queue model is the lever, and the same window shows its spread:

| Queue model | `passive_l2` orders / fills / net PnL | `active_l2` orders / fills / net PnL |
|---|---:|---:|
| `conservative` | 647 / 4 / -0.0205 | 406 / 1 / -0.0020 |
| `prorata` | 652 / 4 / -0.0135 | 381 / 3 / -0.0068 |
| `optimistic` | 652 / 4 / -0.0135 | 279 / 7 / +0.0189 |

These rows were re-measured when the ad-hoc `heuristic` model was replaced, on the same 200,000-row
depth slice at default order sizing, because the earlier numbers belonged to a model that no longer
exists. `prorata` shrinks the queue ahead of a resting order by the fraction the displayed size fell;
`optimistic` treats the whole decrease that way. For `passive_l2`, which always rests at the touch
where the queue ahead starts equal to the displayed size, the two coincide. Neither row predicts live
fills: they are the range that aggregated L2 data leaves open. The only profitable cell is
`optimistic` on `active_l2`, which is a warning about the fill assumption rather than a result.

One scale note, corrected later. `L2MarketMakerConfig::order_size` was `1` and `inventory_limit` was
`1`, because `strategy_factory.cpp` derives both from the instrument's `quantity_scale`:

```cpp
order_size      = max(min_order_quantity, quantity_scale / 1000)
inventory_limit = max(1,                   quantity_scale / 10)
```

That derivation assumes `quantity_scale` encodes an economic scale. For Binance `BTCUSDT` it is
`1000000`, so the L1 workload runs with an order size of 1000 units and an inventory limit of 100000.
For Deribit the depth quantities are already contract counts, so the spec sets `quantity_scale = 1` and
the same derivation produces an order size of **one contract** and an inventory limit of **one
contract**: `10 * 0.5 / 6374 = 0.0008` USD per tick of move, so every net PnL was a few cents and
rounding dominated it. The L2 workload was running 10^6 below the L1 workload's scale, and the strategy
it was describing was not the strategy anyone would have chosen.

`--order-size=N` and `--inventory-limit=N` now override the derivation without changing any existing
command line, verified by the full-file comparison still matching byte for byte. With them the matrix
becomes about something:

| Run, `active_l2`, 200k slice | orders | quantity | fills | realised PnL |
|---|---:|---:|---:|---:|
| default (`order_size` 1), conservative | 406 | 406 | 1 | -0.002 |
| `--order-size=10 --inventory-limit=100`, conservative | 406 | 4060 | 1 | -0.02 |
| `--order-size=100 --inventory-limit=1000`, conservative | 406 | 40600 | 1 | -0.2 |
| default, prorata | 381 | 381 | 3 | -0.0068 |
| `--order-size=10 --inventory-limit=100`, prorata | 408 | 4080 | 3 | -0.0678 |
| `--order-size=10 --inventory-limit=100`, optimistic | 411 | 4110 | 7 | **-0.605** |

Two conclusions. **Order size scales the PnL linearly and never changes the fill count**, under any queue
model: `prorata` fills three times at the default size and three times at ten times the size, with ten
times the PnL, and `conservative` and `optimistic` keep their counts fixed the same way. What sets the
fill count is the queue model. And **at a realistic size the strategy loses more per fill in absolute
terms**, because an order of ten contracts moves the strategy's own inventory skew by ten times as
much. Three fills still prove nothing statistically, but they are now large enough to be about
something, which the previous scale could not express at all.

The size change also exposed a reporting gap, and three fixes went in with it. `[EXECUTION]` printed a
subset of `ExecutionQualityMetrics`, while `[STRATEGY_METRICS]` printed the same quantities from the
strategy's own copy, which only the legacy L1 maker fills; on the L2 path that copy stayed at zero, so
the line showed `captured_edge=0 adverse_selection=0 markout_count=0 max_abs_inventory=0` beside an open
marked position. `[EXECUTION]` now prints the authoritative values from the one place that computes
them. `max_abs_inventory` is now updated where the position changes rather than sampled from a market
view, which had missed a fill arriving as the last event, and `inventory_sign_changes` is counted there
too instead of never. And the markout queue is now drained on the L2 path, which only the L1 path had
done, so adverse selection had been structurally zero on this workload however often the strategy
traded. The legacy maker's positional `StrategyMetrics` initialiser became a named one in passing,
because inserting into that struct silently mis-assigned every field after the insertion point.

With those, the same run answers the question a market maker exists to answer. At
`--order-size=10 --inventory-limit=100` under `prorata` it reports `captured_edge=1.75`,
`adverse_selection=-0.5` and `markout_count=2` of 3 fills, so a third of the edge captured at the fill
is given back five cycles later. Under `conservative` it reports `captured_edge=1.25` and no resolved
markouts, because the one fill arrives too late in the run to have one. The numbers describe the fill
model as much as the strategy: `optimistic` reports `captured_edge=2.75`, `adverse_selection=-1.5` and
`markout_count=6` of 7 fills, which is what the removed `heuristic` produced as well.

Two modelling choices that the earlier notes called limitations are better closed with arithmetic than
with code, because their effect is bounded below anything a run here can measure:

- **Funding is not modelled.** A Deribit perpetual pays funding every eight hours, typically around
  0.01% of notional per interval. A position held for the seconds a maker holds one crosses a boundary
  with probability of order `hold/28800`, so the expected cost is about
  `0.0001 * notional * hold / 28800`. At 10 contracts, $100 of notional, held for a minute, that is
  roughly `2e-6` USD against fills worth about 0.08 USD each. Modelling it would add a rate input that
  is not in the data and whose effect is four orders of magnitude below the PnL it would adjust.
- **Collateral is cash only.** The engine holds no margin and no leverage, so exposure cannot exceed
  cash. That is not an approximation while `inventory_limit * unit_notional <= equity`: at
  `--inventory-limit=100` on a 10 USD contract the maximum position is 1000 USD against 1000 USD of
  starting cash, so the model sits exactly at the boundary where a margin model would begin to matter.
  Past that boundary a run would need one, rather than silently ignoring it.

These are single-run replay reference values, not profitability evidence. Repeat a row with the same
input hashes when checking exact output determinism.

### Queue position at arrival

A resting order is never first in the queue, so `displayed_quantity_ahead` returns the displayed best
size of the side the order joins. It previously returned 0 whenever the order price was not exactly
the best price, which let an order resting behind the touch fill in full the moment any trade printed
through its level. Those fills are selectively optimistic, because they appear exactly when the
market is moving against the resting side. Correcting this removed four of the six fills the window
previously produced; the two that remain are `passive_l2` quotes that sat at the touch, where the
old and new estimates agree.

### Calibrating the queue model against the feed

The fraction of a displayed-size decrease that a queue model honours was the last part of the fill
model resting on a number nobody had measured, so it was measured. `tools/calibrate_queue_model.py`
walks the incremental depth stream, tracks every price level, and attributes each fall in a level's
displayed size to the trades printed at that price on that side between the level's previous update
and that one. Whatever is left over is cancel or amendment.

Over the first 200,000 incremental depth rows, which span 898 seconds of 2020-04-01:

| Quantity | Value |
|---|---:|
| level decreases observed | 98,922 |
| total size removed from levels | 5,162,246,010 |
| explained by trades at the same price | 2,136,580 (0.04%) |
| therefore cancelled or amended | 5,160,109,430 (99.96%) |
| decrease events at least 99% cancellation | 98,268 (99.3%) |
| events where trades exceeded the decrease | 2 (0.0%) |

Two checks say the attribution is not an artefact of the window logic. The trade file holds 1,334
trades and 2,146,690 contracts inside that same 898 seconds, and the windows captured 2,136,580 of
them, or 99.5%, so essentially all of the traded volume is accounted for rather than falling between
updates. And only two of 98,922 decreases had more trade volume attributed to them than the level
actually lost, which is what a misalignment between the two files would produce in bulk.

So the displayed book churns **about two thousand four hundred times the volume that actually
trades**. On this instrument a level's size is mostly a quotation that is withdrawn and replaced: a
resting order's queue ahead of it is dissolved by cancellations far more often than it is consumed by
trades. That is what made the old ad-hoc `heuristic` untenable, since its two constants, a cap on the
reduction it honoured and then a fixed fraction of it, had no reading that matches this measurement,
and it is also why `conservative` should be read as a floor rather than as a neutral assumption.

What the data cannot show is *where* in a level's queue those cancellations sat, because that is not
public. `prorata` therefore assumes they are uniform and shrinks the queue ahead in proportion to the
displayed size, which the two measurable extremes bracket: nothing moves (`conservative`) or all of it
does (`optimistic`). The measurement narrowed the range rather than closing it, and the sensitivity
table above shows what is still at stake in it: three fills against seven on one window, and one
profitable cell out of six.

### Strategy execution correctness pass

Three defects in this area were one class of mistake: a number or a decision that two components each
owned, with nothing comparing them. They were fixed together, and the measurements below are from the
200,000-row depth slice.

**A wrapper that forgot one callback.** `ActiveL2MarketMaker` and `InventoryAwareL2MarketMaker` held an
`L2MarketMaker` and hand-forwarded each of its eight virtuals. `InventoryAwareL2MarketMaker` never
forwarded `on_queue_activity`, which silently disabled two things at once: the base's queue-consumption
counters, which reported zero, and the base's queue-hold refresh policy, which is built on the same
member. A forgotten forward changes behaviour without failing to compile, so both classes now derive
from `L2MarketMaker` and inherit everything they do not override. On this workload the refresh
behaviour barely moved (`price_refresh_count` 221 → 221, age 121 → 120) because the hold only binds at
a long quote age, but the counters went from `buy/sell_queue_consumed=0/0` to `1310/47480`.

**Two copies of every execution number.** `[STRATEGY_METRICS]` printed its own `captured_edge`,
`adverse_selection`, `markout_count`, quote lifetime and inventory fields beside `[EXECUTION]`, and
they disagreed: the same run reported `captured_edge=1.25` and `max_abs_inventory=10` in `[EXECUTION]`
and `0` and `0` in `[STRATEGY_METRICS]`, because only the legacy L1 maker ever filled those fields.
That is the same shape as the four-defect line fixed earlier; the earlier fix made `[EXECUTION]`
authoritative without removing the copy that contradicts it. `StrategyMetrics` now carries only what a
strategy owns (its own submit, cancel, fill, refresh and queue counters) and execution quality exists
once, in `ExecutionQualityMetrics`. The Python benchmark's parser already read only the strategy-owned
fields, so this split matches what the tools expected. The L1 maker still computes its own markout
internals, which its unit tests assert and nothing publishes; deleting that duplicate implementation is
the follow-up, not part of this pass.

**A documented constraint that nothing enforced or measured.** `docs/PERFORMANCE_HISTORY.md` states
that the model is cash only with no margin, so exposure cannot exceed cash. No component enforced it,
reported it, or noticed it. A strategy without an inventory limit (`naive`, which also never cancels)
reached a cash balance of `-120,940 USD` while `equity` still printed `+29%`, and nothing in the
summary said so. Two changes: the summary now reports `max_abs_exposure_usd` against
`starting_cash_usd` and counts fills that pushed the marked exposure past it, and `--max-position=N`
adds a pre-trade gate that counts the quantity already resting on the side an order adds to, because
those orders can still fill. The gate is off by default, so recorded runs are unaffected, and the
default Deribit path is inside it anyway (`max_abs_exposure_usd=100` against `1000`). With the gate on,
the same `naive` run goes from 122,794 fills and `cash=-120,940` to 43,714 fills and `cash=+1,924`.
Without it, the run now says `max_abs_exposure_usd=487,710` and `exposure_over_collateral_fills=122,493`
instead of saying nothing. The L1 BTCUSDT baseline runs at roughly seventy times its cash by the same
measure, which is a property of that fixture and is now visible rather than implied.

**A cross-check that had never been run.** The strategy, the pipeline and the portfolio each track the
position independently. The summary now compares the first two on every report and counts divergences.
On the L2 workload it reports `strategy_position_mismatches=0`, so the three agree, which is what makes
the strategy's own sizing trustworthy. If a callback were dropped again this counter is where it would
show, rather than in a PnL difference nobody can attribute.

**One gap left open on purpose.** `ActiveL2MarketMaker` compares `pause_after_ticks_per_second` against
a mid-price speed built from the time between views, and this feed's view gaps run from microseconds to
hundreds of milliseconds, so that speed measures the gap distribution rather than the market:
`min_elapsed_seconds=0.05` is above the 8.5 ms mean gap, so the floor always applies. Measured, the
halt fires 46 times at the field's value of 9.0 and 246 times at 50.0, and every value in between lands
in the same place. The threshold that would mean something is a tick range over a fixed time window,
which is a behaviour change to a risk control, and it is recorded here as a known gap rather than
half-changed: the field is renamed to say it is a per-second rate, the finding is written next to it,
and the 200,000-row invariants were re-verified unchanged after the rename.

## Interpretation and Next Step

The wall-clock baselines include startup, parsing, simulation, and output I/O; they do not identify
event-level latency. The L1 sample is unsuitable for profitability conclusions, and none of these
numbers constitutes a low-latency performance claim.

Preparation is now complete for the first optimization pass: a paired one-hour performance
baseline, a repeated gprof profile, and a four-strategy regression matrix are recorded. Use
`active_l2` incremental replay as the primary performance target; retain snapshot and the other L2
strategies as compatibility checks. Keep inputs, build flags, and output summaries fixed so
performance changes can be compared without changing behavior.

## Long-Window Profile Result

The latest profile used the current `toy_quant_profile` binary, four one-hour incremental runs,
and `--no-output`, so file output was removed from the sample. The leading application hotspots
were:

| Function | Sample share |
|---|---:|
| `split_csv_views` | 17.3% |
| `L2OrderBook::apply_incremental_batch` | 16.2% |
| `parse_uint64` | 12.1% |
| `DeribitIncrementalBookReader::append_update` | 6.9% |
| `L2OrderBook::market_view` | 6.4% |
| `ActiveL2MarketMaker::on_l2_market_view` | 2.9% |

This changes the next optimization priority: the parser and typed batch/update path are the main
cost centers, followed by L2 map updates. Strategy code is not currently a primary hotspot, and
output I/O was excluded from this profile. The percentages come from an instrumented Debug-style
gprof build and guide prioritization; they are not production latency numbers.

## Buffered Line Reader

The line-reading layer was replaced before any parsing work, because `std::getline` and `gzgets`
are both per-character loops. `src/market/line_reader.*` now pulls 1 MiB blocks from a plain
`std::ifstream` or a `gzFile`, locates line boundaries with `memchr`, and hands back a
`std::string_view` into the block, so the common case neither scans byte by byte nor allocates.
A line that crosses a block boundary is compacted to the front of the block before the next read,
so an owned copy is only needed for a line larger than one block. All five readers
(`BinanceAggTradeReader`, `BinanceBookTickerReader`, `DeribitBookSnapshotReader`,
`DeribitIncrementalBookReader`, `DeribitTradeReader`) use it, which also removed the
comma-counting pre-pass that the old `split_csv(std::string)` needed to size its reserve.

### Line layer in isolation

A standalone microbenchmark over the first 200 MB (2,591,303 rows, about 76 bytes per row) of
`data/v2/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv`, three runs, medians:

| Stage | Seconds | Throughput |
|---|---:|---:|
| Raw 1 MiB block reads only | 0.037 | 5.4 GB/s |
| Block reads plus `memchr` newline scan | 0.076 | 2.6 GB/s |
| `std::getline` into `std::string` | 0.115 | 1.7 GB/s |
| `LineReader::next_line` | **0.080** | **2.5 GB/s** |
| `LineReader` plus the current per-row `find(',')` field split | 0.200 | 1.0 GB/s |

Two conclusions matter more than the 1.44x on the line layer itself:

- `LineReader` is within 10% of the `memchr`-only floor, so the line layer is now close to the
  irreducible read cost. `std::getline` was adding about as much time as the scan itself.
- **Per-row field splitting costs 0.120 s, which is more than the entire pre-change line layer.**
  Eight `find(',')` calls over roughly 10 byte fields are dominated by `memchr` call setup rather
  than by bytes scanned. Within this layer the field split, not the line split, is the next target.
  The stage timings below show that the field split is in fact only a small part of the parser's
  total cost, so this isolation result should not be read as the overall priority.

  **Corrected later:** the `memchr` half of that reasoning is wrong. A byte loop measured the same
  speed as `std::string_view::find`, so this host's `memchr` is not slow for ten byte fields, and the
  only variant that beat it was a word-at-a-time scan, by about 12% — below the measurement floor. See
  [Low-Latency Design](LATENCY_DESIGN.md#a-word-at-a-time-scan-for-the-field-split).

### End-to-end

Workload: the full-day `data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz` (119,238 rows)
replayed against the first 2,000,000 rows of the incremental depth file, `active_l2` and the
conservative queue model. Runtime summary: 1,303,131 events, 1,183,894 incremental batches, one
snapshot batch.

| Build | Median | Range | Paired mean difference |
|---|---:|---|---:|
| Pre-change, with output | 1.52 | 1.39–1.77 | |
| `LineReader`, with output | 1.41 | 1.21–1.64 | **−0.156 s (−10.2%)** |
| Pre-change, `--no-output --fast-validation` | 1.61 | 1.46–2.02 | |
| `LineReader`, `--no-output --fast-validation` | 1.41 | 1.28–1.63 | **−0.258 s (−16.0%)** |

Nine paired runs each, alternating pre-change and `LineReader` so that thermal and system drift
affect both builds equally. **Only the paired difference is trustworthy here.** Unpaired runs of a
single build ranged over 0.38 s, which is larger than the difference between the builds, so an
earlier unpaired comparison on this host reported a misleading `1.46x`.

Behavior is unchanged: `data/runtime/orders.csv`
(`521275515d80052d84f5ffb458cc2b82311168d7d3c7002fdd5d54889540b835`) and `data/runtime/trades.csv`
(`171bddd11a1d60e42ff250b9a0e6b3bedc90a44325d676b88c1303f20b88a02c`) match the pre-change build
byte for byte, and the full stdout/stderr summary is identical.

### Where the end-to-end saving actually came from

The saving is larger than the isolation table predicts, and most of it is not the vectorised scan:

| Source | Estimated share |
|---|---|
| Removing one `malloc`/`free` pair per batch | largest |
| Per-character line scan removal (the isolation table) | about 27 ms of 258 ms |
| Per-row `std::string`/`vector<std::string>` materialisation in `DeribitTradeReader` and the two Binance readers, plus their comma-counting reserve pre-pass | small on this workload |

`read_batch` used `pending_row_ = std::move(row)`. The move steals the row buffer, so the next
`std::getline` has to allocate a fresh one: on this workload that is roughly 1.18M allocate/free
pairs. `pending_row_.assign(view)` reuses the existing capacity instead, so the allocation
disappears. This is worth recording because the profiling story pointed at parsing, and the
largest single win turned out to be allocation churn on the batching boundary.


### Coverage added

`tests/line_reader_test.cpp` covers LF, CRLF, empty lines, a missing trailing newline, a line
larger than one block, block level batching, gzip parity, and line-for-line parity against
`data/v2/test_aggTrades_5k.csv`. `tests/market_data_adapter_test.cpp` gained incremental book
reader coverage for `local_timestamp` batching, the snapshot batch that starts the stream, CRLF
rows, and the header sniffing in `make_deribit_depth_reader`.

Every test source now starts with `#ifdef NDEBUG / #undef NDEBUG / #endif` before its first
include. Without it `linux-relwithdebinfo` compiled the checks out, so a guarded setup call was
skipped while the following `std::get<BboQuote>` still ran and aborted with
`std::bad_variant_access`. That preset is the profiling build, so its test run has to mean
something; the whole suite now executes its checks in both presets.


## Stage Timing (before the container change)

The function level profile above left 38.2% of the sample unattributed, and its percentages are
shaped by the instrumentation itself: gprof adds a call at every function entry, so tiny helpers
are charged for the measurement and inlined helpers are charged to their caller. The stage timings
measure declared regions instead, which is the question that matters here: how long is each step of
turning a row into a quote.

`l2_replay ... --profile-stages[=N]` records eight regions on one event in every `N` (default 64)
and prints exact percentiles to stdout, so it also works together with `--no-output`. With the flag
absent the profiler is not attached and the run is unchanged; that was verified by diffing the full
stdout and the runtime CSV hashes against the pre-change binary.

### Clock cost on this host

Reading the monotonic clock is not the ~20 ns that is normally assumed here:

| Call | ns |
|---|---:|
| `std::chrono::steady_clock::now()` | 71 |
| `clock_gettime(CLOCK_MONOTONIC)` | 72 |
| `clock_gettime(CLOCK_MONOTONIC_RAW)` | 68 |

This host reports itself as WSL2, where these reads are not on a fast path. Eight regions need
sixteen reads per sampled event, about 1.1 microseconds, so timing every event would have cost
more than the code being measured. At an interval of 64 the added wall time is roughly 17 ns per
event, under 1.5% of a 1.3 microsecond event, which is why the instrumented and uninstrumented
runs take the same time.

Because the read cost is significant, the profiler calibrates its own instrument first: it probes
two back to back clock reads at construction and subtracts the minimum from every sample (35-44 ns
here). Without that subtraction each of the eight regions was charged for one clock read, which
inflated the column sum by roughly 400 ns per event and made it appear larger than the total run
time.

### Measured breakdown

Full-day Deribit trades (119,238 rows) against the first 2,000,000 rows of the incremental depth
file, `active_l2` and the conservative queue model, `--no-output --fast-validation
--profile-stages`. 1,303,131 events, 1,183,894 incremental batches, wall clock 1.80-1.85 s, which
is 1381-1420 ns per event.

| Stage | Samples | Share | Mean | p50 | p90 | p99 | p999 | Max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| read+parse | 20361 | **50.4%** | 922.4 | 622 | 766 | 1102 | 58996 | 2835896 |
| validate | 20361 | 5.0% | 85.1 | 62 | 122 | 192 | 860 | 61219 |
| book-apply | 18514 | 15.7% | 287.0 | 193 | 338 | 710 | 3312 | 559972 |
| market-view | 18514 | 9.0% | 145.2 | 111 | 180 | 299 | 1173 | 226713 |
| top-booking | 18514 | 1.9% | 44.3 | 23 | 75 | 125 | 405 | 42885 |
| engine | 18514 | 13.5% | 274.3 | 167 | 354 | 606 | 4569 | 233840 |
| queue-acct | 18514 | 0.6% | 16.7 | 7 | 43 | 76 | 173 | 43463 |
| strategy | 18514 | 3.9% | 122.7 | 48 | 114 | 307 | 7029 | 339400 |
| **column sum** | | 100.0% | 1897.7 | **1233** | | | | |

Three consecutive runs produced the same ordering and the same shares within one percentage point,
so the ranking is stable even though the absolute values track the machine state.

### How to read it

- Share is computed from the p50 column. The mean is not usable for stages that have rare stalls:
  `read+parse` reaches 39-59 microseconds at p999 and 2.8-9.3 milliseconds at its maximum, which is
  a page fault or a 1 MiB block refill showing up in a single sample. Those few samples move the
  mean by hundreds of nanoseconds while leaving the median where it belongs.
- The p50 column sum lands within 10-15% of the per-event wall clock time, so the eight regions do
  cover essentially the whole event. Treat the column sum as a ranking tool and the wall clock as
  the absolute figure.
- `top-booking`, `engine` and `queue-acct` had no row at all in the function level profile. They
  were previously part of the unattributed 38.2%.

### What this changes for the next step

`read+parse` alone is half of the per-event cost. Combining this with the isolation measurement
above, and knowing that a batch here holds one or two rows of about 76 bytes, the split inside one
batch is roughly:

| Part | ns |
|---|---:|
| line reader, two rows | 76 |
| field split, two rows at about 46 ns | 92 |
| number conversion, two rows at about 200 ns | 400 |
| batch assembly, including one `updates` vector allocation | 60 |

**Number conversion is the largest single item in the whole run, near a third of the per-event
cost.** It is two `std::from_chars<double>` and three `parse_uint64` calls per row. This corrects
the priority that the isolation benchmark implied, because that benchmark could only see the line
layer and pointed at field splitting, which is about 7% of the event.

That prediction was right, and acting on it took longer than it should have. Both halves — the
`from_chars<double>` for the price and a hand written integer path for the timestamps — were attacked
and rejected on isolation comparisons that under-measured them. Both were eventually reimplemented as
exactly equivalent fast paths, verified byte-identical on the full file, and accepted: together about
20% of `read+parse`. See [Low-Latency Design section 5](LATENCY_DESIGN.md#5-the-number-conversions) and
the [ledger](PERFORMANCE.md#5-optimization-ledger).

Updated order of work:

1. Number conversion inside the reader: fixed point parsing so that prices become integer ticks
   directly, plus a hand written integer path.
2. `book-apply` and `market-view`, together 25%.
3. `engine`, 13%, which is a larger target than the function level profile suggested.

Validation and strategy are together under 9%, so they stay out of scope for now.

Where that work stands, as recorded at the time: the reader keeps small batches inline and dispatches
columns through a jump table, and a fixed point rewrite of the price parse was measured and dropped
because it did not pay. `book-apply` and `market-view` were later rebuilt on `TickLadder`, and the
engine's per-symbol maps were later re-keyed by integer id. Both are in the
[optimization ledger](PERFORMANCE.md#5-optimization-ledger), with their mechanisms in
[Low-Latency Design](LATENCY_DESIGN.md).
