# User Guide

ToyQuant is a small C++20 market-making simulator. It reads synthetic market ticks, lets a strategy submit quotes, matches orders, and writes the results to CSV files.

## Requirements

- CMake 3.16 or newer
- A C++20-capable compiler
- Python 3 for the optional scenario and UDP tools

## Build and Test

Run these commands from the repository root:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The main programs are `build/toy_quant` for the simulation and `build/backtest_main` for metrics from recorded data.

## Run with CSV

```bash
./build/toy_quant csv data/scenarios/sample_ticks.csv 0 optimized
```

The command format is:

```bash
./build/toy_quant csv [tick_file] [delay_ms] [strategy]
```

`delay_ms` is normally `0`; use a positive value to slow down the tick stream. The available strategies are `naive` and `optimized`, with `optimized` as the default.

## Run with UDP

Start the simulator:

```bash
./build/toy_quant udp 9000 optimized
```

In another terminal, send a scenario:

```bash
python3 tools/udp_sender.py 127.0.0.1 9000 data/scenarios/sample_ticks.csv 0
```

CSV mode is easier for repeatable experiments; UDP mode is useful for observing a streaming feed.

## Inspect Results

Each run updates:

```text
data/runtime/orders.csv
data/runtime/trades.csv
```

The terminal shows tick and summary information. Backtest reports are written under `logs/`:

```bash
./build/backtest_main \
  data/scenarios/sample_ticks.csv \
  data/runtime/orders.csv \
  data/runtime/trades.csv \
  0 0 backtest logs/backtest.log
```

## Troubleshooting

- Run commands from the repository root so relative paths resolve correctly.
- Check that the input CSV exists and uses the expected tick format.
- Use a non-negative integer for the delay and a UDP port between `1` and `65535`.
- If there are no trades, try `shock_ticks.csv` or `random_ticks.csv`; a quiet scenario may not cross the strategy's orders.

For implementation details, see [Architecture](ARCHITECTURE.md). For available market inputs, see [Scenarios](SCENARIOS.md).