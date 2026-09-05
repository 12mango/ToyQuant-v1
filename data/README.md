# Data Directory

This directory has two purposes:

- `scenarios/`: Tick scenarios used as market-data inputs.
- `runtime/`: Generated order and trade outputs.

## Tick Input Format

Scenario files use CSV with a header row:

```csv
ts,symbol,price,size,side
1759080000000,EURUSD,1.18300,58,B
```

| Field | Meaning |
|---|---|
| `ts` | Timestamp in milliseconds. |
| `symbol` | Instrument symbol, for example `EURUSD`. |
| `price` | Tick price. |
| `size` | Tick quantity. |
| `side` | `B` for buy direction and `S` for sell direction. |

## Generate Scenarios

Run this command from the project root:

```bash
python3 tools/gen_ticks.py all --count 1000 --seed 42 --output-dir data/scenarios
```

This command creates files such as:

- data/scenarios/flat_ticks.csv
- data/scenarios/uptrend_ticks.csv
- data/scenarios/downtrend_ticks.csv
- data/scenarios/shock_ticks.csv
- data/scenarios/random_ticks.csv
- data/scenarios/synthetic_ticks.csv

See [Market Scenarios](../docs/SCENARIOS.md) for the purpose of each scenario.

## Runtime Output

The application writes strategy orders and trades to:

- data/runtime/orders.csv
- data/runtime/trades.csv

Both output files use this format:

```csv
ts,symbol,side,price,quantity,order_id
```

`orders.csv` records submitted strategy orders. `trades.csv` records only actual `MarketMaker` trades and is read by `backtest_main`.

The generated built-in scenarios contain 1,000 ticks each, which is enough for a meaningful demo and visualization. `sample_ticks.csv` remains a small 20-tick input for quick format checks.

## Typical Workflow

1. Select or generate a scenario file in `data/scenarios/`.
2. Run the engine with that file.
3. Inspect the generated outputs in `data/runtime/`.
4. Reuse the same scenario and parameters to compare repeated results.

The application's default paths follow this directory layout.
