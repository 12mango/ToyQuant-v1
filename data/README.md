# Data Directory

This directory has two purposes:

- `scenarios/`: Tick scenarios used as market-data inputs.
- `runtime/`: Generated order and trade outputs.

## Legacy Tick Input Format

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

## v2 Trades and BBO Input

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

Replay validates each merged event before it reaches the strategy. Invalid prices or quantities,
crossed BBO, timestamp regression, and non-increasing stream sequence numbers stop the run. The
final `[DATA]` line reports softer cross-stream issues: trades without a prior BBO, trades whose
latest BBO is older than one second, and trades more than 50 basis points from the BBO midpoint.
The line also includes `status=ok` or `status=warning`. These counters diagnose input alignment;
they do not currently suppress strategy decisions.

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

## Slice L2 Snapshots

Large L2 snapshot files should be sliced before tests. The slicer reads rows incrementally and
does not load the source file into memory. Deribit snapshot timestamps are in microseconds:

```bash
python3 tools/slice_l2_snapshot.py \
	data/v2/deribit_book_snapshot_25_2020-04-01_BTC-PERPETUAL.csv \
	/tmp/l2_tiny.csv \
	--max-rows 100
```

Use an inclusive timestamp window or keep every Nth matching row when needed:

```bash
python3 tools/slice_l2_snapshot.py input.csv /tmp/l2_window.csv \
	--start-ts 1585699200000000 --end-ts 1585699500000000 --every 10
```

The same tool accepts Tardis gzip-compressed trades files, so a small paired sample can be made
without first extracting the full file:

```bash
python3 tools/slice_l2_snapshot.py \
	data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
	/tmp/trades_tiny.csv --max-rows 1000
```

## More Deribit Days

The out-of-sample check in `docs/PERFORMANCE_HISTORY.md` compares days of the same policy through the same depth
format, so a new day is two files and one command. Tardis publishes free samples of the first day of each month
for its whole catalogue, and the two datasets this path uses are `book_snapshot_25` and `trades` for
`BTC-PERPETUAL`. Download them from the dataset pages on tardis.dev, which serve samples to a browser session,
and save them here as:

- `deribit_book_snapshot_25_YYYY-MM-DD_BTC-PERPETUAL.csv.gz`
- `deribit_trades_YYYY-MM-DD_BTC-PERPETUAL.csv.gz`

Then one command prints a row per day in the table's column order, together with the recorded block that
`tools/check_docs.py` re-derives:

```bash
bash tools/multi_day_check.sh                 # every day found under data/v2/
bash tools/multi_day_check.sh 2020-06-01      # or the days named on the command line
```

It cuts the same 200,000-row slice per day, refuses a file that is not a complete gzip rather than reading a
prefix of it, and warns when a day yields fewer rows than the others, because a short slice would otherwise look
like a quiet day rather than a short download.

## Inspecting a New Source

A new source is a set of assumptions, and the wrong ones are quiet: microseconds read as milliseconds, a price
that is still a string, or an incremental file taken for a snapshot. `tools/inspect_source.py` prints what a file
actually contains before any reader exists for it — delimiter, header, column count, inferred types and ranges,
which integer columns are timestamps and in what unit, whether they move forward, and the top of book implied by a
JSON level column:

```bash
python3 tools/inspect_source.py data/v2/bitmex_trades_2020-06-01_XBTUSD.csv.gz
```

The column summary is the anatomy half of accepting a source, and the implied top of book is the book half, which
is what a reader's output gets compared against. It reads the first rows by default, so it is safe on a
multi-gigabyte file, and it reads the gzip stream once to say whether a download is complete rather than
interpreting a partial file as a short day.

## Runtime Output

The application writes strategy orders and trades to:

- data/runtime/orders.csv
- data/runtime/trades.csv

Legacy CSV and UDP runs begin with `# source_ticks=...`; Trades+BBO replay runs begin with
`# source_market_data=...`. Both then use the common output columns:

```csv
ts,symbol,side,price,quantity,order_id
```

`orders.csv` records submitted strategy orders. `trades.csv` records only actual `MarketMaker` trades and is read by `backtest_main`.

Legacy runtime files include the source Tick path so `backtest_main` can detect a mismatch between
the Tick file used to generate trades and the Tick file supplied for marking prices. Replay metadata
records both market-data paths and instrument settings; the current legacy backtest analyzer does
not consume full BBO replay output. Older files without metadata remain compatible.

The generated built-in scenarios contain 1,000 ticks each, which is enough for a meaningful demo and visualization. `sample_ticks.csv` remains a small 20-tick input for quick format checks.

## Typical Workflow

1. Select or generate a scenario file in `data/scenarios/`.
2. Run the engine with that file.
3. Inspect the generated outputs in `data/runtime/`.
4. Reuse the same scenario and parameters to compare repeated results.

The application's default paths follow this directory layout.
