# User Guide

ToyQuant is a small C++20 market-making simulator. Its current v2 path replays Binance aggregate
trades and BBO updates as `MarketEvent` values, while legacy CSV and UDP modes remain available for
simple scenarios and compatibility.

**This document is the how-to-run reference**: modes, flags, inputs, outputs and troubleshooting. It does not
explain *why* the software is shaped the way it is — that is [Design Review](DESIGN_REVIEW.md) — and it is not
the module-by-module implementation reference, which is [Architecture](ARCHITECTURE.md).

## Requirements

- CMake 3.16 or newer
- A C++20-capable compiler
- Python 3 for the optional scenario and UDP tools

## Build and Test

Run these commands from the repository root:

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

The same code also builds and tests under AddressSanitizer and UndefinedBehaviorSanitizer, which is the
gate's tier 3 (`bash tools/verify_l2.sh asan`):

```bash
cmake --preset linux-asan
cmake --build out/build/linux-asan
ctest --preset linux-asan
```

The main programs are `out/build/linux-debug/toy_quant` for simulation/replay and
`out/build/linux-debug/backtest_main` for legacy metrics from recorded Tick data.

## Current Modes

```text
csv         -> legacy Tick scenario
udp         -> legacy Tick stream
replay      -> v2 Binance Trade+BBO MarketEvent replay
l2_replay   -> Deribit L2 trade + depth replay, the mainline path
carry       -> two leg carry measured over two funding histories
```

`l2_replay` is the mainline path and the source of every market-making number the documents quote: it is the only
path that models the queue standing in front of a quote, and a fill depends on that queue rather than on the price
alone. `replay` (Binance Trade+BBO) and the legacy CSV and UDP inputs are retained for compatibility and for
the pinned regression invariants, and their fill counts answer a different question. `carry` measures the funding
line instead and is described at the end of this guide. The detail is in the L1
note in `docs/ARCHITECTURE.md`.

## Run with Legacy CSV

```bash
./out/build/linux-debug/toy_quant csv data/scenarios/sample_ticks.csv 0 optimized
```

The command format is:

```bash
./out/build/linux-debug/toy_quant csv [tick_file] [delay_ms] [strategy]
```

`delay_ms` is normally `0`; use a positive value to slow down the tick stream. The available strategies are `naive`, `optimized`, and `l1`, with `optimized` as the default.

## Run with Legacy UDP

Start the simulator:

```bash
./out/build/linux-debug/toy_quant udp 9000 optimized
```

In another terminal, send a scenario:

```bash
python3 tools/udp_sender.py 127.0.0.1 9000 data/scenarios/sample_ticks.csv 0
```

CSV mode is easier for repeatable experiments; UDP mode is useful for observing a streaming feed.
Both modes use the legacy `Tick` contract and are not equivalent to the Binance Trade+BBO replay.

## Replay Trades and BBO

Replay the Binance samples in `data/v2` with:

```bash
./out/build/linux-debug/toy_quant replay \
  data/v2/test_aggTrades_5k.csv \
  data/v2/test_bookTicker_5k.csv \
  BTCUSDT 0 optimized 1000000
```

The command format is:

```text
toy_quant replay <trades_csv> <bbo_csv> <symbol> [delay_ms] [strategy] [quantity_scale]
```

The replay feed streams and merges both files by transaction timestamp. BBO events replace the
external best bid and ask and trigger strategy decisions. Aggregate trades drive fills; Binance's
`is_buyer_maker=true` means that the seller was the aggressor.

Each event is validated before dispatch. Invalid structural data stops the run. The final `[DATA]`
summary reports `status=ok` or `status=warning`; warning means the replay contained soft alignment
issues such as a missing BBO, stale BBO, or a trade far from the BBO midpoint.

`quantity_scale` converts decimal exchange quantities to the engine's integer units. The default
is `1000000`, so `0.001 BTC` becomes `1000` internal units. Strategy order sizes and generated
runtime quantities use those same units. BTCUSDT replay also uses its instrument specification for
the `0.10` price tick, minimum quantity, fee metadata, strategy spread, and inventory units. The
existing `csv` and `udp` modes retain their legacy defaults.

Use `l1` when you want the BBO-aware strategy:

```bash
./out/build/linux-debug/toy_quant replay \
  data/v2/test_aggTrades_5k.csv \
  data/v2/test_bookTicker_5k.csv \
  BTCUSDT 0 l1 1000000
```

### L2 strategies and validation

Use Deribit trades and reconstructed top-25 depth snapshots with the L2 replay mode. The available
strategy names are:

| Name | Role |
|---|---|
| `passive_l2` | Depth-aware reference baseline |
| `inventory_aware_l2` | Inventory-control comparison |
| `flow_aware_l2` | Flow/depth comparison |
| `active_l2` | Comprehensive L2 mainline |
| `adaptive_l2` | Compatibility alias for `active_l2` |

Run the standard benchmark with a fixed window and `depth_every=1`:

Use the same benchmark configuration for every candidate strategy:

```bash
python3 tools/benchmark_l2_strategies.py \
  --binary build/toy_quant \
  --trades data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  --depth data/v2/deribit_book_snapshot_25_2020-04-01_BTC-PERPETUAL.csv \
  --symbol BTC-PERPETUAL \
  --start-ts 1585699200000000 \
  --end-ts 1585699500000000 \
  --depth-every 1
```

To replay the connected incremental L2 file, pass it as the depth argument:

```bash
./build/toy_quant l2_replay \
  data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  data/v2/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv \
  BTC-PERPETUAL 0 active_l2 1
```

The reader groups rows by `local_timestamp` and emits one decision per update batch. Snapshot
depth files remain supported through the original full-snapshot reader.

Two optional flags override the sizing that is otherwise derived from the instrument's quantity scale
(`order_size = quantity_scale/1000`, `inventory_limit = quantity_scale/10`). For an instrument whose
quantities are already contract counts that derivation is wrong: `BTC-PERPETUAL` declares
`quantity_scale = 1` and therefore ran with an order size of one contract and an inventory limit of one,
which makes every PnL a few cents and the inventory logic unreachable.

```bash
./build/toy_quant l2_replay \
  data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  data/v2/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv \
  BTC-PERPETUAL 0 active_l2 1 conservative --order-size=10 --inventory-limit=100
```

The remaining `l2_replay` flags are `--no-output`, `--fast-validation`, `--profile-stages[=N]` and
`--live-pacing[=SPEED]`.

The symbol decides the instrument, through `instrument_spec_of`: `BTC-PERPETUAL`, `BTC-USDT-SWAP`, `SPY` and
`MESZ6` are the entries today, and a symbol with no entry is an error rather than a default, because a default
would quietly price the wrong tick with the wrong fee. A listed future also states its contract multiplier,
which is what makes one micro E-mini S&P 500 unit worth five times the index price, and its quarter point tick
worth 1.25 USD rather than 0.25, so a commission of about sixty cents a contract is half a tick a side:

```bash
./build/toy_quant l2_replay data/v2/glbx_MESZ6_trades.csv.gz data/v2/glbx_MESZ6_depth.csv.gz \
  MESZ6 0 active_l2 1 conservative --order-size=5 --inventory-limit=20 --base-spread-ticks=1
```

The collateral fields stay a cash-only figure, which is the documented model and not a description of a margined
future: such a run reports exposure over collateral and should be read for its capture, fee and markout columns.

`--live-pacing[=SPEED]` delivers the replay against the wall clock instead of as fast as the file reads, which is
what turns a measurement into a rehearsal: a strategy that watches its own quote age, or cancels on a timer, only
behaves like itself when the market arrives at its real speed. The fifth positional delay still works and now
means "pace this run at real time", because a fixed sleep after every event neither reproduced the file's timing
nor scaled. Pacing changes no decision — only when the decision is taken — so a paced run reports the same
`[PNL]` fields as the unpaced one, and the line it adds is the feed's own report card:

```text
[LIVE] paced_events=3582893 late_events=3551778 lateness_mean_us=6827 lateness_p50_us=1024
       lateness_p99_us=65536 lateness_max_us=79282 market_window_s=7199 wall_s=11 speed=600
```

`lateness_*` is how late each delivery was against the schedule the file's own timestamps define, so it is the
sum of everything between the schedule and the delivery, and it is the one number here that a live session would
watch: at 600 times real time the process above is at its limit and says so, and at real time the same window is
six hundred times less work than that.

When the benchmark is given an incremental file, the tool reconstructs a compact top-five timeline
from the grouped updates and reports the same output fields. Python `markout_5/20` means the
mid-price difference at the fifth/twentieth subsequent sampled depth record; it is not the same
as the C++ runtime markout measured in quote cycles, and results with different `depth_every`
values are not directly comparable. The reconstructed timeline is a comparison aid; it is not an
order-level queue reconstruction.

For incremental feeds, active L2 quote age advances only when the external best-price midpoint
moves by at least one tick. Depth-only quantity updates do not consume quote lifetime. In four
valid direct 15-minute windows on the 2020-04-01 incremental file, active L2 reached `-172.15` net
PnL versus `-218.82` for `flow_aware_l2`; this is a relative improvement, not a profitability claim.

When the queue ahead is actively being consumed and fair price has not changed, active L2 also
preserves the quote through the normal age limit instead of cancelling and losing queue priority.
On four queue-aware incremental windows, this version measured `-133.55` versus `-148.21` for
`flow_aware_l2`; both remain negative after fees.

The main comparison metrics are:

- `net_pnl` — final outcome metric
- `fill_rate` and `cancel_rate` — execution quality and churn control
- `queue_ahead_consumed` — L2 market volume consumed ahead of newly resting maker orders
- `buy_markout_20` and `sell_markout_20` — directional fill quality
- `price_refresh_count`, `age_refresh_count`, `risk_pause_count` — cancellation causes
- `buy_queue_consumed`, `sell_queue_consumed` — queue progress by quote side
- `buy_queue_ahead_levels_cleared`, `sell_queue_ahead_levels_cleared` — price levels whose external queue reached zero
- `buy_fill_count`, `sell_fill_count` — side-specific fill counts
- `buy_fill_probability`, `sell_fill_probability` — fills divided by submitted quotes on each side
- `buy_queue_per_quote`, `sell_queue_per_quote` — queue consumption per submitted quote
- `incremental_snapshot_batches` — snapshot/reset batches observed in incremental input
- `avg_markout_20` — whether fills are taken at favorable markout
- `avg_quote_age_us` — quote freshness and turnover
- `against_depth` and `against_flow` — directional quality of fills
- `audited_orders`, `audited_filled_orders`, `audited_cancelled_orders` — runtime order lifecycle counts
- `cancelled_after_fill_orders`, `cancelled_before_fill_orders` — cancellation outcome split
- `total_order_lifetime_cycles` — quote-cycle lifetime sum, not elapsed microseconds

A candidate must remain competitive across at least two moderate windows; a one-window PnL win is
not sufficient. Use `analyze_l2_windows.py` for repeated 15-minute windows:

```bash
python3 tools/analyze_l2_windows.py \
  --binary build/toy_quant \
  --trades data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  --depth data/v2/deribit_book_snapshot_25_2020-04-01_BTC-PERPETUAL.csv \
  --symbol BTC-PERPETUAL \
  --start-ts 1585699200000000 \
  --end-ts 1585715400000000 \
  --window-minutes 15 \
  --depth-every 1 \
  --csv reports/l2_windows.csv
```

The Deribit benchmark uses `0.02%` maker and `0.05%` taker fee assumptions. Results are
educational replay measurements, not profitability claims. L2 replay uses a fixed one-market-event
cancellation delay; it is not a millisecond latency model. Queue model defaults to `prorata`, the model the measured cancel share supports;
`prorata` and `optimistic` are sensitivity analyses only, with `prorata` chosen because
`tools/calibrate_queue_model.py` measures 99.96% of the size removed from a book level as cancellation
rather than trade. Pass `--queue-model` to `analyze_l2_windows.py` when comparing these assumptions.

The replay also reports `max_abs_exposure_usd` next to `starting_cash_usd`, the cash the run began with.
The model is cash only and no margin, so exposure above that ceiling is a modelling violation, and
`exposure_over_collateral_fills` counts how often a fill pushed the marked position past it. Pass
`--max-position=N` to enforce the ceiling as a pre-trade gate in quantity units; it counts the quantity
already resting on the side an order adds to, so two quotes that arrive in the same cycle cannot jump
it. `strategy_position_mismatches` compares the strategy's own position against the pipeline's on every
report, which is how a dropped callback would show up instead of as an unexplained PnL difference.

The replay has policy levers and model levers, and every one of them is additive to the recorded
defaults, so a run without them reproduces the recorded numbers byte for byte:

| Flag | Effect | Default |
|---|---|---|
| `--order-size=N` | Contracts per quote | Derived from the instrument quantity scale |
| `--inventory-limit=N` | Position at which the strategy stops adding | Derived from the instrument |
| `--max-position=N` | Pre-trade gate the pipeline enforces | Off, reported only |
| `--base-spread-ticks=N` | Quote placement; 1 puts the quote at the venue touch | 2 |
| `--refresh-price-ticks=N` | How far the fair price must move before a requote | 2 for the flow strategies |
| `--arrival-share=X` | Share of the displayed size a new quote starts behind | 1.0, which is FIFO |
| `--queue-chunks=N` | Whole orders per level decrease, for `--queue-model=lumpy` | 1 |
| `--queue-seed=N` | Makes a lumpy run reproducible | Fixed |
| `--order-latency-us=N` | One-way latency between sending an order and the exchange having it | 0, a synchronous engine |
| `--cancel-latency-us=N` | One-way latency between sending a cancel and the exchange applying it | 0, a synchronous engine |
| `--trace-order=N` | Trace the queue in front of order N, reported as `[QUEUE_TRACE]` lines | 0, no trace |
| `--funding-rates=FILE` | Apply an instrument's funding history as the replay passes each settlement | Off, and the summary then has no `[FUNDING]` line |
| `--maker-fee-rate=R` | Replace the venue's maker rate, for the fee question; the run echoes it in its `[Mode]` line and may be negative for a rebate | Off, the venue's own rate |
| `--markout-cover` | Quote only where the capture also covers what passive fills at that distance have cost, measured from our own resolved fills | Off |
| `--flatten-band=N` | Reduce a position above N units with a taker order at the touch instead of carrying it | Off |
| `--live-pacing[=SPEED]` | Deliver the replay against the wall clock at this speed and report how late it ran as `[LIVE]`; the fifth positional argument on this mode means the same at real time | Off, which reads the file as fast as it can |

### Funding as an input

A perpetual pays funding for as long as a position is held, and that is a flow rather than a price, so it
arrives as a file instead of in the event stream:

```bash
./out/build/linux-debug/toy_quant replay \
  data/v2/test_aggTrades_5k.csv data/v2/test_bookTicker_5k.csv BTCUSDT 0 l1 1000000 \
  --funding-rates=data/v2/BTCUSDT-fundingRate-2024-03.csv
```

Two free formats are recognised by their header: an OKX daily funding archive
(`instrument_name,funding_rate,funding_time`, every swap on the venue in one file) and a Binance Vision
monthly `fundingRate` archive (`calc_time,funding_interval_hours,last_funding_rate`, one symbol).
`tools/fetch_funding_rates.py` fetches either for one instrument and `data/README.md` records what the two
files this checkout holds are.

The rate in both is the rate longs pay. The pipeline applies each settlement at the first event at or after
it, on the positions as they stand and at the last price the run has seen, which is the nearest causal mark
available because the settlement instant is not in the event stream. The run then reports:

```text
[FUNDING] funding_points=93 funding_span_hours=736 funding_sum_rate=0.0314019
          funding_annualized_rate=0.373751 funding_settlements=88 funding_flow_usd=0
```

`funding_points`, `funding_span_hours`, `funding_sum_rate` and `funding_annualized_rate` describe the file —
what a long holding one unit of notional paid over its span, and that sum continued over a year — while
`funding_settlements` counts the settlements the window reached and `funding_flow_usd` is what they moved in
cash. The two differ by design: a window that begins after a settlement applies it to an account that holds
nothing yet, so it is counted and worth zero, which is what the line above shows for a five minute replay
that starts inside a month of history. The run above and a ten minute window that does hold a position at a
settlement are both recorded with their commands in
[Performance History](PERFORMANCE_HISTORY.md#funding-rates-as-an-input). A file with no settlement for the
instrument is an error rather than an empty input, because a wrong symbol would otherwise look like an
instrument that never paid funding. Without the flag the summary is unchanged byte for byte, which is what
keeps every recorded run reproducible.

To read one order's life instead of the run's totals, name it and filter the summary for the trace block.
Order ids are assigned in submission order and start at 1, so a small id is an early quote:

```bash
./build/toy_quant l2_replay data/trades.csv.gz data/depth.csv BTC-PERPETUAL 0 active_l2 1 prorata \
  --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --trace-order=3 | grep QUEUE_TRACE
```

Each row is one event that changed the queue standing in front of that order, and the last row is the fill,
the cancel or the completion that ended it. The events are `submit` for the strategy's intent, `rest` for
the queue the order then joined behind, `view` and `trade` for the market events that moved that queue, and
`fill`, `fully_filled` or `cancel` for the report that ended the order. `docs/LEARNING.md` reads one of
these traces line by line.

An order id is not something a reader knows in advance, so a traced run also prints `[TRACE_HINTS]`: the id
of the first order that traded, the id of the filled order that waited longest, and the id of the still
resting order that has waited longest. Quoting the first few ids is usually the wrong way to find a good
example, because the earliest orders are the ones that die in the same millisecond they arrive:

```bash
./build/toy_quant l2_replay data/trades.csv.gz data/depth.csv BTC-PERPETUAL 0 active_l2 1 prorata \
  --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --trace-order=1 | grep TRACE_HINTS
```

Of these, the requote threshold moves the fill count the most of anything measured so far, and the
arrival share the least; `docs/PERFORMANCE_HISTORY.md` records the measured ordering.

### Replay strategy choices

The replay command continues to accept these frozen L1 strategy names for compatibility and
historical result reproduction. They are not an active development target; the current strategy
mainline is `active_l2`.

| Name | Use when you want to demonstrate |
|---|---|
| `passive_l1` | A simple passive two-sided L1 baseline |
| `inventory_aware_l1` | Inventory-sensitive prices and quantities |
| `flow_aware_l1` | Recent trade-flow and BBO imbalance response |
| `l1` | The complete replay-oriented strategy and default mainline comparison |

For a side-by-side replay comparison, use the benchmark target:

```bash
./out/build/linux-debug/strategy_benchmark \
  data/v2/test_aggTrades_5k.csv \
  data/v2/test_bookTicker_5k.csv BTCUSDT 1000000
```

To keep one continuous strategy state while recording hourly checkpoints, append the optional
timeline path after the parameter list:

```bash
./out/build/linux-debug/strategy_benchmark \
  data/v2/BTCUSDT-aggTrades-2024-03-30.csv \
  data/v2/BTCUSDT-bookTicker-2024-03-30.csv BTCUSDT 1000000 \
  0.6 0.4 0.06 0.08 0.20 1 0.60 2.5 0.10 0.40 \
  reports/strategy_timeline.csv
```

The timeline keeps the replay state continuous and records cumulative hourly orders, fills,
position, gross/net PnL, fees, filtered trades, markout, and inventory metrics. The existing
window tool resets state per window and remains useful for comparing market phases; it is not a
replacement for this continuous timeline.

### Current demo status

The replay demo retains historical L1 and current L2 roles:

- `l1`: frozen L1 replay reference with inventory, flow, volatility, and fee-aware spread controls;
- `active_l1`: separate higher-activity experiment focused on inventory rotation;
- `flow_aware_l1`: bounded order-flow experiment and comparison baseline;
- `passive_l1` and `inventory_aware_l1`: simple reference baselines;
- `active_l2`: current L2 mainline for depth-aware replay;
- `passive_l2`, `inventory_aware_l2`, and `flow_aware_l2`: L2 comparison baselines.

Replay trades without a usable BBO, or with a price more than 5 bps from the latest BBO midpoint,
are counted as `filtered` and are not sent to matching. This keeps data-alignment problems from
creating synthetic fills. A high filtered count is still a data-quality warning: it does not mean
the source file has been repaired.

For final comparisons, read `gross_pnl`, `fees`, and `net_pnl` together with `markout`, inventory,
and filtered-trade counts. The result is an educational replay measurement, not a claim of live
profitability.

The benchmark reports submitted orders, filled quantity, final position, gross PnL, fees, net
PnL, and fee ratio. It also records unified execution-quality diagnostics for every replay strategy:
captured edge, adverse selection, inventory, markouts, and quote lifetime. `net_pnl` is the primary
result for cost-aware comparisons;
`gross_pnl` and `fees` explain why it changed.

Every row carries `fills_are_conclusive`: a strategy with fewer than `MINIMUM_FILLS` fills has too
little data for a PnL or per-fill edge statement, and the benchmark prints that warning rather than
leaving the reader to notice the count. On the recorded Deribit window no strategy clears it, which is
the honest reason the project reports a range across queue models instead of one number.

The replay strategies are intentionally different teaching baselines, not production algorithms.
L1 implementations are frozen; `flow_aware_l1` is an experimental transition strategy
whose useful bounded behavior has partly been incorporated into `l1`. `active_l1` is a separate
experiment that produces more observable activity and inventory rotation while accepting more
adverse-selection risk. After a fill it rebuilds quotes around the new inventory on the next BBO
cycle, making the inventory-reversion behavior visible in the benchmark diagnostics.

## Carry: Two Legs Over Two Funding Histories

A carry is a position and its hedge, so it needs two instruments and it does not need a book: what it earns is
funding and what it pays is a round trip of fees. The mode states the position and measures both:

```bash
./out/build/linux-debug/toy_quant carry \
  data/v2/BTCUSDT-fundingRate-2024-03.csv \
  data/v2/okx_funding_2024-03-01_2024-03-31_BTC-USDT-SWAP.csv \
  --symbol-a=BTCUSDT --symbol-b=BTC-USDT-SWAP --notional-usd=10000
```

| Flag | Effect | Default |
|---|---|---|
| `--symbol-a=SYM`, `--symbol-b=SYM` | Which instrument each leg holds; the venue's fee schedule is looked up by symbol | Required |
| `--notional-usd=N` | USD held on each leg, so the position is a hedge of 2N gross and zero net | 10000 |
| `--long=a`/`--long=b` | Which leg is bought; the other is sold | `a` |

The window is whatever the two histories cover together, the position is opened at its first settlement and
closed at its last, and each settlement inside the window is charged to its own leg at its own mark. The
position is stated rather than chosen: `--long` fixes the side before the run looks at the data, so the number
is a measurement and not a fitted result. Entry and exit are taker fills at the venues' published rates, which
is the expensive assumption; a maker entry would halve the fee and is not assumed.

The report is one line:

```text
[CARRY] carry_leg_a=BTCUSDT carry_leg_b=BTC-USDT-SWAP carry_notional_usd=10000
        carry_gross_notional_usd=20000 carry_net_notional_usd=0 carry_window_hours=728
        carry_settlements=182 carry_leg_a_funding_usd=-304.656 carry_leg_b_funding_usd=322.241
        carry_funding_usd=17.5849 carry_fees_usd=18 carry_net_usd=-0.415093
        carry_annualized_carry_usd=211.599 carry_annualized_return=0.0211599
        carry_to_fees=11.7555 carry_breakeven_hours=745.184
```

`carry_gross_notional_usd` and `carry_net_notional_usd` are the two ways to read the same book and only the
second is the account's exposure: both legs carry a notional, and their sum is what the position is worth to
someone who reads one leg as the position. The per-leg funding shows what flows and what is left after the
hedge takes its side back: 304.66 USD paid on one venue and 322.24 received on the other leave 17.58 USD, which
is the carry. `carry_net_usd` is that minus the fees, `carry_annualized_carry_usd` is the carry as a yearly
rate, and `carry_to_fees` divides it by the fees: the acceptance number. `carry_breakeven_hours` is the same
comparison in the form a decision needs, because the fees are paid once and the carry accrues every interval —
here the position must be held 745 hours to pay for its fees and it was held for 728, which is why the net is
just below zero. The run is recorded with its numbers in
[Performance History](PERFORMANCE_HISTORY.md#the-acceptance-number).

The replay then reports a `[PNL]` line for every run, which splits what completed from what is still open and
adds the fees back to each:

```text
[PNL] gross_pnl_usd=-0.0818127 net_pnl_usd=-0.123813 gross_closed_pnl_usd=-0.0672835
      closed_pnl_usd=-0.109283 maker_notional_usd=210 net_per_fill_usd=-0.00589584
      gross_closed_per_fill_usd=-0.00320398 breakeven_maker_rate=-0.000389584
```

`closed_pnl_usd` is the portfolio's realized PnL, so a position the run was still holding at the end is not in
it; `net_pnl_usd` adds that position marked at the last price; the `gross_` pair adds the fees back to each, and
a negative gross PnL says the run lost money before the venue charged anything. `maker_notional_usd` is what the
maker fees were charged on, and `breakeven_maker_rate` is the maker rate at which this window's total PnL would
have been zero: **a negative answer means no fee at all would still have lost money**, and that size of rebate
would have been needed instead. `tools/fee_sweep.sh` runs the sweep over the windows the documents quote and
prints the table with the block that re-derives it.

## Scenarios

Scenarios are CSV files containing synthetic market ticks. They make it easy to run the same
strategy against different market conditions. Built-in files live in `data/scenarios/`:

| File | Market behavior |
|---|---|
| `sample_ticks.csv` | Small input for a quick run |
| `synthetic_ticks.csv` | General-purpose mixed sample |
| `flat_ticks.csv` | Mostly stable prices |
| `uptrend_ticks.csv` | Rising prices |
| `downtrend_ticks.csv` | Falling prices |
| `shock_ticks.csv` | Sudden price movement |
| `random_ticks.csv` | Noisy price movement |

The generated scenarios contain 1,000 ticks each; `sample_ticks.csv` is intentionally small for
quick format checks. Keep the scenario file fixed when comparing `naive` and `optimized`, and
remember that runtime CSV files are overwritten by the next simulation.

Generate reproducible scenarios with a fixed seed:

```bash
python3 tools/gen_ticks.py all --count 1000 --seed 42 --output-dir data/scenarios
```

Using the same seed recreates the same ticks. For a controlled experiment, keep the scenario and
seed fixed, change only the strategy or one parameter, and compare the orders and trades. These
scenarios are for learning and regression experiments; they do not represent real market data or
production trading performance.

## Generate a Visual Report

After running a scenario, create a static report with the three main views:

- `Quotes & Executions`: reference tick price, buy/sell quote bands, and execution markers.
- `Quote Distance to Reference Mid`: quote displacement from the reference price in ticks.
- `Net Inventory Profile`: signed inventory and the configured `+/-1000` inventory boundary.

The report also includes a right-side summary panel with order count, executions, traded quantity,
fill rate, initial capital, current equity, max drawdown, inventory peak, and unexecuted orders.

Set up the optional plotting environment and generate the report with:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python tools/plot_report.py \
  --ticks data/scenarios/sample_ticks.csv \
  --orders data/runtime/orders.csv \
  --trades data/runtime/trades.csv \
  --output reports/sample_ticks_optimized.png
```

The report is a visual diagnostic, not a full PnL report. The input files contain one price per tick rather than complete bid/ask snapshots, so `Reference Mid` uses the tick price as its visible reference. Runtime CSV files are overwritten by the next simulation.

## Inspect Results

Each run updates:

```text
data/runtime/orders.csv
data/runtime/trades.csv
```

Legacy CSV/UDP runtime files begin with `# source_ticks=...`; reuse that same Tick file for
`backtest_main`. Replay runtime files begin with `# source_market_data=...` and are not currently
the input format for the legacy backtest analyzer.

The terminal shows tick and summary information. Backtest reports are written under `logs/`:

```bash
./out/build/linux-debug/backtest_main \
  data/scenarios/sample_ticks.csv \
  data/runtime/orders.csv \
  data/runtime/trades.csv \
  0 0 backtest logs/backtest.log
```

`backtest_main` currently expects the legacy Tick file for mark prices. Replay mode records
`source_market_data` metadata, but full BBO-based portfolio marking is a separate future step.

## Troubleshooting

- Run commands from the repository root so relative paths resolve correctly.
- Check that the input CSV exists and uses the expected tick format.
- Use a non-negative integer for the delay and a UDP port between `1` and `65535`.
- If there are no trades, try `shock_ticks.csv` or `random_ticks.csv`; a quiet scenario may not cross the strategy's orders.

For implementation details, see [Architecture](ARCHITECTURE.md). Data file details are documented
in `data/README.md` in the repository.