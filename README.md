<div align="center">

# ToyQuant

**A reproducible C++20 market-making simulator — from ticks to trades to PnL.**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![CMake](https://img.shields.io/badge/CMake-3.16+-064F8C?logo=cmake&logoColor=white)](CMakePresets.json)
[![Tests](https://img.shields.io/badge/tests-CTest-brightgreen)](tests)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

[User Guide](docs/USER_GUIDE.md) · [Architecture](docs/ARCHITECTURE.md) · [Data Formats](data/README.md)

</div>

---

ToyQuant replays market data through the complete trading loop — market events, order book, strategy, matching, execution reports, and portfolio metrics — so you can observe every decision a market maker makes.

It is a toy project for learning and experimentation, not a production trading system. APIs, scenarios, and strategy behavior may change between versions.

## Demo

```console
$ ./out/build/linux-debug/toy_quant csv data/scenarios/sample_ticks.csv 0 optimized

[Mode: Legacy CSV] Opening: .../data/scenarios/sample_ticks.csv (delay: 0ms) strategy=optimized
...
[EXECUTION] submitted_orders=24 submitted_quantity=1302 cancel_requests=9 trade_reports=11
            fill_rate=0.318 cancel_rate=0.375 trade_report_quantity=414 working_orders=6
```

The same input and parameters produce deterministic runtime CSV output for this simulator.

## How It Works

```text
 Legacy CSV ──╮
 Legacy UDP ──┼─► Tick adapter ─► Pipeline ─► Strategy ─► Matching Engine
 Binance replay ─► MarketEvent ──╯                              │
 Deribit L2 replay ─► L2OrderBook ─► L2MarketView ──────────────╯
                                                               ▼
                                             orders.csv / trades.csv
                                                               │
                                             backtest_main ─► PnL · equity · drawdown
```

- **Four current modes** — legacy CSV scenarios, legacy UDP ticks, Binance Trade+BBO replay, and Deribit L2 trade+depth replay.
- **L2 strategy variants** — `l2_baseline`, `l2_depth`, `l2_micro`, and `l2_flow` separate simple baseline, depth, micro-price, and trade-flow experiments behind the same execution lifecycle.
- **One v2 event path** — Binance replay uses `MarketEvent`; CSV and UDP remain compatibility inputs for the older `Tick` model.
- **Price–time priority matching** with self-trade prevention and partial fills.
- **Stateful execution reports** — position and working orders update from trade, cancel, and fill events.
- **Deterministic backtests** — realized/unrealized PnL and a reusable drawdown calculation.
- **Small toolchain** — CMake, a C++20 compiler, and Python 3 for the optional scenario and report tools.

## Quick Start

Requirements: CMake 3.16+, a C++20-capable compiler, and Python 3.

```bash
cmake --preset linux-debug
cmake --build --preset linux-debug
ctest --preset linux-debug
```

**Run a legacy CSV scenario** (flat, trending, shock, and random markets ship in [data/scenarios](data/scenarios)):

```bash
./out/build/linux-debug/toy_quant csv data/scenarios/flat_ticks.csv 0 optimized
```

**Stream legacy ticks over UDP** (and send a scenario from another terminal):

```bash
./out/build/linux-debug/toy_quant udp 9000 naive
python3 tools/udp_sender.py --port 9000
```

**Replay Binance Trade+BBO data**:

```bash
./out/build/linux-debug/toy_quant replay \
  data/v2/test_aggTrades_5k.csv \
  data/v2/test_bookTicker_5k.csv \
  BTCUSDT 0 optimized 1000000
```

**Replay Deribit L2 trades and snapshots** (slice the large snapshot first):

```bash
python3 tools/slice_l2_snapshot.py \
  data/v2/deribit_book_snapshot_25_2020-04-01_BTC-PERPETUAL.csv \
  /tmp/deribit_depth.csv --start-ts 1585699200000000 --every 5 --max-rows 300
./out/build/linux-debug/toy_quant l2_replay \
  data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  /tmp/deribit_depth.csv BTC-PERPETUAL 0 l2_flow 1
```

**Analyze legacy generated order and trade records**:

```bash
./out/build/linux-debug/backtest_main \
  data/scenarios/sample_ticks.csv \
  data/runtime/orders.csv \
  data/runtime/trades.csv \
  0 0 backtest logs/backtest.log
```

Generate your own scenarios with a fixed seed for reproducible runs:

```bash
python3 tools/gen_ticks.py all --count 1000 --seed 42 --output-dir data/scenarios
```

## Output

Each `toy_quant` run creates or truncates:

- `data/runtime/orders.csv` — every order the strategy submitted.
- `data/runtime/trades.csv` — every execution report the engine returned.

Legacy CSV/UDP runs include a `# source_ticks=...` metadata line. Replay runs include
`# source_market_data=...` with the trade and BBO sources. `backtest_main` reads the legacy
`source_ticks` format; older runtime files without metadata remain readable.

The default backtest log is `logs/backtest.log`.

## Visual Report

Generate a static report with the market price, strategy quotes, executions, net inventory, and run summary:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python tools/plot_report.py \
  --ticks data/scenarios/synthetic_ticks.csv \
  --orders data/runtime/orders.csv \
  --trades data/runtime/trades.csv \
  --output reports/synthetic_ticks_optimized.png
```

The generated PNG is designed for quick inspection and README screenshots. Runtime CSV files are overwritten by the next simulation, so generate or copy the report before starting another run.

![ToyQuant simulation report](reports/synthetic_ticks_optimized.png)

## Non-Goals

ToyQuant deliberately excludes real exchange connectivity, FIX, a risk gateway, persistence and recovery, nanosecond-latency claims, and full L2 market reconstruction. Keeping these out of scope is what keeps the core loop small enough to read in one sitting.

## Documentation

| Document | Contents |
|---|---|
| [User Guide](docs/USER_GUIDE.md) | CLI reference, scenarios, experiments, data contracts, and troubleshooting |
| [Architecture](docs/ARCHITECTURE.md) | Data flow, module responsibilities, matching rules, order lifecycle |
| [Data Files](data/README.md) | Tick CSV format, runtime outputs, typical workflow |

## Contributing

Bug reports, documentation improvements, and focused tests are welcome. Please open an issue before proposing new features.

## Disclaimer

This project is for educational purposes only. It does not provide investment advice and must not be used for live trading.

---

<div align="center">
Created and maintained by <a href="https://github.com/12mango">12mango</a> · <a href="mailto:1498159938@qq.com">Contact</a> · <a href="LICENSE">MIT License</a>
</div>
