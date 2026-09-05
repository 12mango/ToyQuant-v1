# Scenarios

Scenarios are CSV files containing synthetic market ticks. They make it easy to run the same strategy against different market conditions.

## Available Files

All built-in files are in `data/scenarios/`. The generated scenarios contain 1,000 ticks each; `sample_ticks.csv` is intentionally kept small for quick format checks.

| File | Market behavior |
|---|---|
| `sample_ticks.csv` | Small input for a quick run |
| `synthetic_ticks.csv` | General-purpose mixed sample |
| `flat_ticks.csv` | Mostly stable prices |
| `uptrend_ticks.csv` | Rising prices |
| `downtrend_ticks.csv` | Falling prices |
| `shock_ticks.csv` | Sudden price movement |
| `random_ticks.csv` | Noisy price movement |

## Run a Scenario

Pass the file to `toy_quant` in CSV mode:

```bash
./build/toy_quant csv data/scenarios/shock_ticks.csv 0 optimized
```

Replace the filename to compare scenarios. Replace `optimized` with `naive` to compare the two strategies. After each run, inspect the terminal summary, `data/runtime/orders.csv`, and `data/runtime/trades.csv`.

The runtime CSV files are overwritten on the next run, so copy them first when comparing results.

## Generate Scenarios

The generator creates reproducible input files:

```bash
python3 tools/gen_ticks.py all --count 1000 --seed 42 --output-dir data/scenarios
```

Use the same `--seed` to recreate the same ticks. For a useful comparison, keep the scenario and seed fixed, change only the strategy or one parameter, and compare the orders and trades.

These scenarios are for learning and regression experiments. They do not represent real market data or production trading performance.