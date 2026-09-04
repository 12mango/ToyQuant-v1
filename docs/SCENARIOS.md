# Market Scenarios

Scenario files live in `data/scenarios/` and provide repeatable inputs for strategy runs and backtests.

| File | Market behavior | What to observe |
|---|---|---|
| `flat_ticks.csv` | Flat market | Quotes should remain near a stable price and inventory should not grow persistently in one direction. |
| `uptrend_ticks.csv` | Rising market | The smoothed mid price and quotes should move upward. |
| `downtrend_ticks.csv` | Falling market | The smoothed mid price and quotes should move downward. |
| `shock_ticks.csv` | Sudden price movement | Quote refreshes, cancellations, and inventory-limit responses. |
| `random_ticks.csv` | Random noise | Strategy stability under noisy input. |
| `synthetic_ticks.csv` | Default mixed sample | Default data for a quick demo or backtest. |
| `sample_ticks.csv` | Small sample | Debugging the CSV format and console output. |

## Run a Scenario

```bash
./build/toy_quant csv data/scenarios/shock_ticks.csv 0 optimized
```

The second optional argument is the delay in milliseconds between ticks. Use `0` for fastest processing, or a positive value to make console output easier to inspect.

## Generate Scenarios

```bash
python3 tools/gen_ticks.py all --count 100 --seed 42 --output-dir data/scenarios
```

Use a fixed `--seed` to generate identical data and reproduce a strategy or backtest result.

## Inspect Results

After each run, inspect:

- `[TICK]` and `[SUMMARY]` in the console.
- Strategy order records in `data/runtime/orders.csv`.
- Trade records in `data/runtime/trades.csv`.
- Realized PnL, unrealized PnL, equity, and maximum drawdown in the backtest log.

Scenarios are intended to make system behavior understandable, not to establish real-market performance or strategy profitability.
