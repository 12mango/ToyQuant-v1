# User Guide

ToyQuant is a small C++20 market-making simulator. Its current v2 path replays Binance aggregate
trades and BBO updates as `MarketEvent` values, while legacy CSV and UDP modes remain available for
simple scenarios and compatibility.

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

The main programs are `out/build/linux-debug/toy_quant` for simulation/replay and
`out/build/linux-debug/backtest_main` for legacy metrics from recorded Tick data.

## Current Modes

```text
csv    -> legacy Tick scenario
udp    -> legacy Tick stream
replay -> v2 Binance Trade+BBO MarketEvent replay
```

`replay` is the current primary path. CSV and UDP are intentionally retained as lightweight
compatibility/demo inputs rather than separate v2 market-data models.

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

### Replay strategy choices

The replay command accepts these strategy names:

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

The replay demo is complete for strategy comparison and learning. The current roles are:

- `l1`: defensive mainline with inventory, flow, volatility, fee-aware spread, and execution-quality metrics;
- `active_l1`: separate higher-activity experiment focused on inventory rotation;
- `flow_aware_l1`: bounded order-flow experiment and comparison baseline;
- `passive_l1` and `inventory_aware_l1`: simple reference baselines.

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

The five replay strategies are intentionally different teaching baselines, not five production
algorithms. `l1` is the mainline strategy; `flow_aware_l1` is an experimental transition strategy
whose useful bounded behavior has partly been incorporated into `l1`. `active_l1` is a separate
experiment that produces more observable activity and inventory rotation while accepting more
adverse-selection risk. After a fill it rebuilds quotes around the new inventory on the next BBO
cycle, making the inventory-reversion behavior visible in the benchmark diagnostics.

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