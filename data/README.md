# Data directory

This folder is split into two purposes:

- scenarios/: input tick files used as market-data scenarios.
- runtime/: generated outputs from a run, such as orders.csv and trades.csv.

## Scenario generation

Run the scenario generator from the project root:

```bash
python3 tools/gen_ticks.py all --count 100 --seed 42 --output-dir data/scenarios
```

This creates files such as:

- data/scenarios/flat_ticks.csv
- data/scenarios/uptrend_ticks.csv
- data/scenarios/downtrend_ticks.csv
- data/scenarios/shock_ticks.csv
- data/scenarios/random_ticks.csv
- data/scenarios/synthetic_ticks.csv

## Runtime output

The application writes its generated orders and trades into:

- data/runtime/orders.csv
- data/runtime/trades.csv

## Typical workflow

1. Generate or choose a scenario file in data/scenarios.
2. Run the engine or backtest using that file.
3. Inspect generated outputs under data/runtime.
4. Reuse the scenario file for repeatable tests.

The default app paths are aligned to this layout.
