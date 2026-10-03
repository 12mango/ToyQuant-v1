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
	data/v2/crypto/deribit_book_snapshot_25_2020-04-01_BTC-PERPETUAL.csv \
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
	data/v2/crypto/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
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
python3 tools/inspect_source.py data/v2/crypto/bitmex_trades_2020-06-01_XBTUSD.csv.gz
```

The column summary is the anatomy half of accepting a source, and the implied top of book is the book half, which
is what a reader's output gets compared against. It reads the first rows by default, so it is safe on a
multi-gigabyte file, and it reads the gzip stream once to say whether a download is complete rather than
interpreting a partial file as a short day.

## IEX HIST: US Equity Depth, and Where It Stops

**The route is dropped, and the reasons are the useful part.** DEEP is an order-level feed — order adds,
executions, deletes and cancels, each carrying an order reference — rather than the aggregated price levels this
model consumes, so using it would mean a decoder written against a specification that has to be obtained, and
then a second layer aggregating order events into the ladder the book and the queue model are built on: a new
lineage rather than an extension of one. It is also IEX's own book, so it cannot be made into a national best
bid, and it is not a venue an account opened from Hong Kong can rest on, which means its depth can neither rank a
symbol (the screen measures the venue we would trade) nor validate a fill. The equity depth this repository does
use came from the paid Databento route above, at about two dollars a session.

The equity market is the first venue here where the fee can be smaller than a tick, so it is the market the fee
question needs an answer from, and its free depth is a daily archive rather than an endpoint.
`tools/fetch_iex_hist.py` prints that route and then verifies whatever the operator downloaded:

```bash
python3 tools/fetch_iex_hist.py --plan --date 2026-09-29
python3 tools/fetch_iex_hist.py --verify data/v2/iex/20260929_IEXTP1_DEEP1.0.pcap.gz --sample 4
python3 tools/fetch_iex_hist.py --self-test      # also run by tier 1 of the gate
```

What the IEX market-data page says, read on 2026-09-30: HIST is available for download "on a T+1 basis", the
binary output of each feed is "provided in pcap format", it is captured via the lower level IEX Transport
Protocol, and "firms must use appropriate specification versions to decode each pcap". The example name on that
page is `20170515_IEXTP1_DEEP1.0.pcap.gz`, which is date, protocol version, feed and specification version.
DEEP is the depth feed and TOPS is the top of book only, and the file list on that page has a Login column, so
the download is behind an account and an acceptance of the IEX Historical Data Terms of Use rather than an
endpoint a script can call unattended.

That is why the tool does not fetch. `--verify` reads the container instead: the gzip stream first, then the pcap
global header and records, then IPv4 and UDP, then the IEX-TP segment prefix, and it counts the message type
bytes it finds. It prints the packet count, the first and last packet time, the ports, the protocol versions, the
message lengths and whether a segment holds one message or several — all measured on the file, none assumed.
`--sample N` prints the raw first bytes of N messages, which is what a decoder gets written against, and
`--self-test` builds such a file and checks the walk against it, because the archive itself cannot be fetched in
a test.

**What the tool will not do is decode a DEEP message.** The published specification no longer answers at the URL
this repository knew, and a decoder written from memory would produce a book that looks right and is not, so the
type bytes and lengths above are the anatomy half and the decoder is the next half. A decoder is not planned: the
route is dropped for the reasons at the top of this section, and the equity depth this repository measures on is
the paid ten-level one above. The tool is kept because the container walk is real work whose self-test tier 1 of
the gate runs, and because it is the first half of any decoder a future route would need.

The instrument half is in place already: `us_equity_spec` states the one cent tick, the share scale and the
commission per share, `docs/ARCHITECTURE.md` explains why a per-share commission is a field of its own rather
than a rate, and the registered symbol is looked up by the same `instrument_spec_of` the carry mode uses.

## Databento L2: A Tradable Market With Real Depth

The depth a market-making test needs is ten price levels per side plus the trades, and the first source here that
is both that and a market a Hong Kong personal account may trade is Databento's `mbp-10`, which
`tools/fetch_databento.py` downloads and converts. The key comes from `DATABENTO_API_KEY` and is never written to
a file in this checkout:

```bash
export DATABENTO_API_KEY=db-...
python3 tools/fetch_databento.py --cost --dataset GLBX.MDP3 --schema mbp-10 --symbol MESZ6 \
  --start 2026-09-29T13:30 --end 2026-09-29T15:30
python3 tools/fetch_databento.py --download --dataset GLBX.MDP3 --schema mbp-10 --symbol MESZ6 \
  --start 2026-09-29T13:30 --end 2026-09-29T15:30 --pretty-px \
  --output data/v2/futures/glbx_mbp10_MESZ6_2026-09-29_1330_1530.csv
python3 tools/fetch_databento.py --convert --input data/v2/futures/glbx_mbp10_MESZ6_2026-09-29_1330_1530.csv \
  --symbol MESZ6 --exchange glbx --depth-out data/v2/futures/glbx_MESZ6_depth.csv.gz \
  --trades-out data/v2/futures/glbx_MESZ6_trades.csv.gz
```

What the route is, read on 2026-09-30 from the API itself rather than from a page: the endpoints take their
parameters **in the query string**, and a JSON body is rejected with every field reported missing, which is what
the first version of the tool did for an hour. The `mbp-10` schema carries **the whole ten level book on every
row**, with `action` describing what happened at the top, so one download is both the depth and the trades: the
trades are the `action=T` rows and cost nothing extra, where a separate trade feed is twenty-eight dollars a
gigabyte on the same venue. Prices are integers scaled by 1e-9 unless `--pretty-px` asks for decimals, and the
bill is on the DBN size rather than the CSV size, so a busy two hours of the micro E-mini is about half a dollar
and a session is about two. The depth file itself is several gigabytes of CSV, which is why it stays untracked.

The conversion writes the CSV pair the existing readers already consume — the Deribit incremental book shape and
the Deribit trade shape — so no C++ was added for this format. The converter checks its own work twice on the way
through, because a conversion is the kind of step where a plausible bug is invisible:

* it rebuilds every row's book from the deltas it emitted and compares that against the ten levels the vendor
  sent, which came out exact over 2.98 million rows of one download;
* it counts the aggressor convention rather than assuming it: of the trade rows in one slice, 43,467 printed at
  the ask with `side=B` and 44,160 printed at the bid with `side=A`, with 3,678 inside or beyond the touch, so
  `B` is the buy and the price agrees with the label on every trade that had a touch to agree with. The
  convention is a convention, and the counts are what make it a measured one.

Two facts about the file that a reader has to know, both measured here rather than assumed: the vendor's
`sequence` **ties** across the rows one event produces, so the trade file's `id` column is the converter's own
running index because the reader requires strict increase; and `local_timestamp` is the vendor's receive time, so
rows are grouped into batches by a clock that can repeat between events, which the reader handles by applying the
batch in order.

A third fact is a decision: Databento labels a trade with the side of the aggressing order, and uses `N` where
there is no aggressor to name. Those prints cannot drive a model whose fills and markouts are keyed by the
aggressor, so the converter **drops them and counts them**: 4,294 of the 28,592 prints in the SPY two hours, and
none in the futures file. Dropping them removes trades the queue model would otherwise consume, which is the
conservative direction, and the count is printed on every conversion so a file that was mostly dropped is
visible rather than quietly thinner.

The raw instrument, before conversion, is one contract of a micro E-mini S&P 500 future: a quarter point tick
worth 1.25 USD because the contract multiplier is 5, and a commission charged per contract. The same tool takes
an equity: SPY on the Nasdaq book, one cent tick, sizes in shares, 2.18 million depth rows and 28,592 trade rows
in two hours, and the same two hours again for **AAPL (786,111 depth rows and 39,254 trade rows) and TSLA
(425,234 and 35,466)**, the two symbols the screen below passed. `tools/f4_windows.sh` re-runs all four pairs, and
`docs/ROADMAP.md` records what the runs found.

## Screening a Symbol Before Buying Its Depth

`tools/screen_spread.py` is the cheap half of the phase 2b instrument screen: it prices a Databento window
before it spends anything, and it refuses to download without a cap, so a screen cannot surprise anyone. It reads
the top of book only, which is why it runs on the cheap schema:

```bash
export DATABENTO_API_KEY=db-...
python3 tools/screen_spread.py --cost --symbols SPY,QQQ,IWM --start 2026-09-29T13:30 --end 2026-09-29T15:30
python3 tools/screen_spread.py --download --max-cost-usd=5 --symbols SPY,QQQ,IWM \
  --start 2026-09-29T13:30 --end 2026-09-29T15:30
python3 tools/screen_spread.py --input data/v2/equities/xnas_mbp10_SPY_2026-09-29_1330_1530.csv --symbol SPY
python3 tools/screen_spread.py --input data/v2/equities/xnas_mbp10_SPY_2026-09-29_1330_1530.csv --symbol SPY \
  --min-trades-per-second=2.8
```

The last form runs on a file already on disk, which is how the tool is tested without spending anything, and it
accepts a ten-level file because that schema carries the top of book columns as well. Downloads are cached under
`data/v2/screen/`, named by symbol and window, so a symbol that has been screened once can be re-measured for
nothing.

**The cheap schema is a way to rank a symbol and not a substitute for the ten-level one, and that was measured
rather than assumed** (2026-10-02, and it cost nothing because the AAPL file for the recorded window was already in
the cache): converted by `tools/fetch_databento.py --convert` and run through the same policy as the ten-level
file, top of book gives **1,182 fills against 2,324** and **67,157 submitted orders against 45,907**. The engine's
own output says why -- with one level, a book whose touch is taken has nothing left to become the new touch, so
quotes land behind the touch 23,148 times instead of 11,596 -- which is also why this script is only ever read for
its spread, its trade counts and its cost: the strategy runs on ten levels, and a top-of-book file buys a different
measurement rather than the same one for less.

**The rule has a second form now, and it is the first one with a price in it.**
`--fee-schedule=min=0.35,regulatory=0.0000206,clearing=0.0002,audit=0.000003,rebate=0.0013` is the same string the
engine takes, and with it the tool prints `price`, `schedule_ticks`, `round_turn_ticks`, `schedule_ratio` and a
second verdict beside the commission-only one: the median spread at the trades against three times the round turn of
the account's own terms at that share's price. The commission-only verdict stays printed, because the recorded table
was measured under it and has to stay re-derivable. The minimum per order is reported and deliberately not charged
here: it is charged per order instance rather than per share, so it depends on how often a policy replaces its
quotes, which top of book cannot see, and it is measured in the engine on whatever symbol passes. Re-running the
cached thirty symbols under the rule costs nothing, and `docs/ROADMAP.md` records what it decided.

What it reports, and why it reports two distributions:

* **the median book spread next to the median spread at the trades.** They are not the same number, and the second
  is the one that matters: SPY's book has a median of 2.000 ticks over its recorded two hours while the rows where
  a trade prints have a median of 1.000. A fill happens where a trade happens, so a rule written against the book
  would rank a symbol by a spread nobody trades at.
* **the ratio of the half spread at the trades to the fee per side**, which is the pre-registered rule: three or
  more passes. It has now been run on thirty symbols for 0.52 USD: **AAPL passes at 4.286 and TSLA at 7.143**, the
  first two symbols to pass either half of the gate, while SPY fails at 1.429 and the micro E-mini at 1.008. The
  funds whose spreads are widest fail the activity floor instead -- JNUG at 43.571 with 0.016 trade prints a second
  against the floor's 2.8 -- and the screen's own table is in `docs/PERFORMANCE_HISTORY.md`.
* **the trade count, the trades per second and the rows per second.** The activity floor is recorded in
  `docs/ROADMAP.md` and derived there from the fill counts of the first two screened symbols, and the screen
  applies it when it is given `--min-trades-per-second`: a run that way prints `trades_per_second` and its own
  verdict beside the spread verdict, so the number a symbol is judged against was written down before that
  symbol's answer was in. The tool does not carry the floor, so it cannot quietly become a different one.

## OKX Trades, and What the Public Route Cannot Do

The public history endpoint answers without a key, and `tools/fetch_okx_trades.py` turns it into one CSV per
symbol per day:

```bash
python3 tools/fetch_okx_trades.py --date 2023-03-01 --symbol BTC-USDT-SWAP
```

What that route serves, checked against the endpoint itself: `module=1` is tick trades, `2` is candles, `3` is
funding rates, and `11` is margin borrow rates. **There is no order book module**, so the L2 dataset advertised on
the historical data page is reachable only through the web portal and has to be downloaded by hand.

Two properties of the trades file matter before it is joined to anything else. The day is cut on midnight UTC+8, so
the file named `2023-03-01` begins at `2023-02-28 16:00` UTC, and `size` is in contracts, so a linear swap whose
contract value is `0.01` needs that multiplication before the amount is a base amount. The script prints both
rather than leaving them in a comment, because both shift a day or a size by a constant without failing.

The order book archive is a different shape from the trade archive, and the readers reflect that. The portal hands
it over as a `.data` member inside a `tar.gz`, and every line is a JSON object:

```text
{"instId":"BTC-USDT-SWAP","action":"snapshot","ts":"1767744000007","asks":[["93713","248.89","26"],...],"bids":[...]}
{"instId":"BTC-USDT-SWAP","action":"update","ts":"1767744000017","asks":[["93739.1","0.01","1"]],"bids":[...]}
```

* The first line is a full depth snapshot and the rest are updates carrying only the levels that changed, where a
  size of zero removes a level, which is the amount the book removes on.
* The day is cut on midnight **UTC**, unlike the trade archive's UTC+8, so pairing the two needs both files.
* There is no sequence number and no local receive timestamp anywhere in the file. A gap therefore cannot be
  detected from the file itself, and a batch is stamped with the exchange timestamp in both stamp fields.
* A long file re-sends a full snapshot periodically — five of them in the first three hundred thousand records —
  so the snapshot's ordering token is its timestamp, which is the only monotone key the archive carries.
* Applying every update since a snapshot does **not** reproduce the next one exactly: on the first three hundred
  thousand records, 39 of about 800 levels disagree, in ways that look like a level leaving or entering the
  400-level window and like a small size difference inside it. The archive is therefore not a lossless record of
  the top of the book, and a reader must not assume that updates reconstruct a snapshot. The first snapshot itself
  is exact, which is what a reader's output is compared against. `tools/check_okx_book.py` performs both checks.
* `size` is in contracts and the price is quoted with a quote character around it, so the reader removes the quote
  and multiplies by the contract value before the quantity reaches the engine.

```bash
tar -xzOf data/v2/crypto/BTC-USDT-SWAP-L2orderbook-400lv-2026-01-07.tar.gz | head -n 300000 > /tmp/okx_l2_300k.jsonl
./out/build/linux-debug/toy_quant l2_replay <okx trades csv> /tmp/okx_l2_300k.jsonl BTC-USDT-SWAP 0 \
  active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --order-size=100000
```

The venue is taken from the depth file rather than from the command line, so the same command runs either the OKX
archive or the Deribit files this repository holds.

## Funding Rates

The carry line's second input, and the smallest file this project reads: a funding history is a few megabytes
per symbol per year even at an hourly settlement. `tools/fetch_funding_rates.py` fetches one instrument from
either free source and writes it here:

```bash
python3 tools/fetch_funding_rates.py --source okx --date 2026-01-07 --symbol BTC-USDT-SWAP
python3 tools/fetch_funding_rates.py --source okx --from 2024-03-01 --to 2024-03-31 --symbol BTC-USDT-SWAP
python3 tools/fetch_funding_rates.py --source binance --month 2024-03 --symbol BTCUSDT
```

That produces `data/v2/funding/okx_funding_2026-01-07_BTC-USDT-SWAP.csv` (3 settlements,
`instrument_name,funding_rate,funding_time`), `data/v2/funding/okx_funding_2024-03-01_2024-03-31_BTC-USDT-SWAP.csv`
(93 settlements, one file day cut and concatenated per day of the range) and
`data/v2/funding/BTCUSDT-fundingRate-2024-03.csv` (93 settlements, `calc_time,funding_interval_hours,last_funding_rate`).
The March 2024 pair is what the acceptance number is measured on, because a carry needs two histories that cover
the same window and Binance publishes monthly while OKX publishes daily. Six properties decide how a series joins
a run:

- **The OKX day is cut on midnight UTC+8**, like its trade archive, so the file named `2026-01-07` holds the
  settlements at `2026-01-06 16:00`, `2026-01-07 00:00` and `08:00` UTC, while the book archive for the same
  date is cut on UTC and starts at `00:00`. A settlement at UTC midnight therefore sits at the first record of
  the book archive and at the file boundary of the funding archive, and the script prints the offset on every
  run rather than leaving it in a comment.
- **The OKX file carries the whole venue.** It is one CSV for every swap OKX lists, so the reader slices the
  requested instrument out of it and a symbol the file does not mention yields an empty series. Consecutive file
  days tile exactly for one instrument, which is what lets a range be concatenated into a single series.
- **Binance publishes funding rates monthly, not daily.** `fundingRate/` has no daily folder, unlike
  `premiumIndexKlines`, which is a different series (the premium index, not the rate). One archive is one
  symbol-month.
- **The Binance row declares its own interval** (`8` for BTCUSDT) and `calc_time` is a settlement timestamp in
  milliseconds, with a few milliseconds of jitter: several rows end in `...000001`, so the series is eight
  hours apart to within a second rather than exactly. `tests/funding_rate_test.cpp` asserts that tolerance and
  says why, because a strict equality would be a red test about the venue's clock.
- **The rate is what longs pay.** Both venues publish the funding rate in that direction, which is the opposite
  of the rate `Portfolio::accrue_interval_flow` takes, so the sign is flipped once, where the pipeline applies
  the settlement.

A run consumes the file with `--funding-rates=FILE` and reports a `[FUNDING]` line. `docs/PERFORMANCE_HISTORY.md`
records two runs over these files with the command that re-derives each number, and `docs/USER_GUIDE.md` explains
the line's fields.

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
## What is in `data/v2` now, and what was removed

The directory is deliberately **not** split into family subdirectories: every file below is named by an exact path in
the gate, in a checked run block, or in this document's own provenance table, and moving one would silently break the
reference that keeps it honest. What it gets instead is this map.

| family | files | size | what it is | regenerable |
|---|---:|---:|---|---|
| `deribit_*` | 8 | 4.6 GB | the 2020 perpetual snapshots, incremental books and trades the L2 path is measured on | yes, from the Tardis samples |
| `BTCUSDT-*` | 15 | 1.4 GB | Binance aggTrades, bookTicker and funding archives for March 2024 | yes, from Binance Vision |
| `xnas_*` | 29 | 1.8 GB | the US equity windows: depth and trades per symbol and session | yes, from Databento, about two dollars a session |
| `glbx_*` | 3 | 2.0 GB | the micro E-mini session, raw and converted | yes, from Databento |
| `BTC-USDT-SWAP-*` | 1 | 415 MB | the OKX book archive for one day | yes, from the OKX archive |
| `screen/` | 60 | 1.2 GB | the top-of-book files of the thirty symbol screen, cached so the rule can be re-measured for nothing | yes, but it is the cache that makes the re-measurement free |
| `*funding*` | 25 | 148 KB | both venues' funding histories for 2024, which closed the carry line | yes, and they are free |
| `test_*_5k.csv` | 2 | 2.7 MB | the small pinned inputs the replay tests use | no, they are fixtures |

**Removed, and why**: eighteen `*:Zone.Identifier` files (Windows alternate-data-stream metadata rather than data), an
empty `v2/carry/` directory, and four archives no gate or checked block names — `BTCUSDT-bookTicker-2024-03-30` and the
June, July and August 2020 Deribit snapshots — for **873 MB**. Each is re-fetchable with the commands above, and every
file a checked block re-derives its numbers from was deliberately left alone.
