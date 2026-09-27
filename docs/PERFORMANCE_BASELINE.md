# Performance Baseline

This document records a lightweight, reproducible end-to-end baseline before any latency-focused changes. It is a regression reference, not a production throughput or latency claim.

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

All four retained L2 strategies were benchmarked on the same 15-minute trade window for each
depth source, with `depth_every=1` and the conservative queue model. These summaries are
per-source regression references; do not compare snapshot and incremental PnL or fills as if the
event streams were identical.

| Strategy | Incremental orders / fills / net PnL | Snapshot orders / fills / net PnL |
|---|---:|---:|
| `passive_l2` | 657 / 2 / -13.056 | 641 / 2 / -13.056 |
| `inventory_aware_l2` | 420 / 2 / -4.961 | 408 / 4 / -9.410 |
| `flow_aware_l2` | 414 / 1 / 0.980 | 363 / 1 / -21.021 |
| `active_l2` | 411 / 1 / 0.980 | 393 / 1 / 0.980 |

These are single-run replay reference values, not profitability evidence. The matrix exists to
detect behavioral drift if a shared hot path is later optimized; repeat a row with the same input
hashes when checking exact output determinism.

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
