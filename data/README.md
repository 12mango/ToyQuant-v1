# Data Directory

This directory has two purposes:

- `scenarios/`: Tick scenarios used as market-data inputs.
- `runtime/`: Generated order and trade outputs.

## Tick Input Format

Scenario files use CSV with a header row. Generated scenario files contain the header directly:

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
| `side` | `B` for buy, `S` for sell; it may be empty and then becomes `Unknown`. |

The parser requires the first four fields, accepts an optional `side` field, and ignores extra
columns so the input can be extended without changing the core fields.

## Trades and BBO Input

`v2/` contains a Binance aggregate-trade file and a book-ticker BBO file. Replay mode consumes the
files together rather than converting them into the legacy Tick format:

```text
toy_quant replay <aggTrades.csv> <bookTicker.csv> <symbol> [delay_ms] [strategy] [quantity_scale]
```

The Binance aggregate-trade columns are aggregate trade ID, price, quantity, first trade ID, last
trade ID, transaction time, buyer-is-maker, and best-match. A file may optionally have an
`agg_trade_id` or `aggregate_trade_id` header. Book ticker uses its named header with update ID,
best bid price/quantity, best ask price/quantity, transaction time, and event time.

Transaction time controls cross-file ordering. Quantities are multiplied by `quantity_scale`
(default `1000000`) and rounded to unsigned integer engine units.

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

See the [User Guide](../docs/USER_GUIDE.md#scenarios) for the purpose of each scenario.

## Runtime Output

The application writes strategy orders and trades to:

- data/runtime/orders.csv
- data/runtime/trades.csv

Legacy CSV runs begin with `# source_ticks=...`; Trades+BBO replay runs begin with
`# source_market_data=...`. Both then use this format:

```csv
ts,symbol,side,price,quantity,order_id
```

`orders.csv` records submitted strategy orders. `trades.csv` records only actual `MarketMaker` trades and is read by `backtest_main`.

Each runtime file begins with a `# source_ticks=...` metadata line. The backtest uses it to detect a mismatch between the Tick file used to generate the trades and the Tick file supplied for marking prices. Older files without this metadata remain compatible.

The generated built-in scenarios contain 1,000 ticks each, which is enough for a meaningful demo and visualization. `sample_ticks.csv` remains a small 20-tick input for quick format checks.

## Typical Workflow

1. Select or generate a scenario file in `data/scenarios/`.
2. Run the engine with that file.
3. Inspect the generated outputs in `data/runtime/`.
4. Reuse the same scenario and parameters to compare repeated results.

The application's default paths follow this directory layout.
