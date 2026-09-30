# Roadmap

Two goals, one codebase: a demo that shows the engineering, and the option of a personal execution system.
**The demo comes first**, so the phases are ordered by what a reader can see, and each step is chosen so that
it also keeps the later path open instead of paying for it now.

The seam that makes both possible already exists: `Strategy`, `IMatchingEngine` and `IOrderBook` are the only
things the pipeline knows about, and `src/market/` holds one reader per input format. A live feed is another
reader, and paper trading is the existing book and matching engine against that feed.

## Where the project stands

| | |
|---|---|
| Delivered | the queue model, the fee model, the two latency legs, the order trace and the queue distribution, four sweeps of the strategy levers, and a four-tier gate |
| The binding result | at a one-tick spread the fee is 2.57 ticks against 0.088 captured, and no lever moves that ratio |
| Frozen | the pre-queue-model line is tagged `v1.0-demo` |
| Open | the conclusions rest on one day, and nothing has been run against a live feed |

## The phases

| Phase | Work | What it buys | Cost |
|---|---|---|---|
| **0 — packaging** | one branch, the `v1.0-demo` tag, the documents in the published navigation, a code CI | a reader sees a maintained repository with a history rather than two unfinished ones | done |
| **1 — evidence** | the headline conclusions re-run on a second day, which the repository already holds, and the queue-model bounds experiment recorded | closes the two questions the documents could not answer: does it generalize, and how much does the queue model matter. Both are now recorded, and the second day's numbers are behind a marked block in the gate | done |
| **2 — the live paper milestone** | a websocket book feed, a paper-trading mode reusing the book and the matching engine, and a recorder that writes the live feed in the replay format | the strongest statement available here: one strategy drives a replay and a live session, and a live session becomes replayable | one to two weeks |
| **2a — a second venue** | a reader for the OKX historical format, added to `src/market/` beside the existing ones, plus a reconciliation check that the rebuilt book matches an independent recomputation of the top of book | consecutive-day free data with discrete levels, which is what a multi-day test needs; the existing pinned lineage is untouched because a new venue is an additional workload | done: the readers exist, `market_data_format_of` routes a depth file to the right one, and a paired replay of one hour of the 2026-01-07 archive completes with the summed order book, the exchange's fee schedule and the contract size of 0.01 BTC |
| **3 — system only** | persistence and recovery, the pre-trade gate enabled by default, with a check that the limit exceeds one order, a real venue adapter at small size, multi-instrument accounting | none of it is checkable by a reader, so it waits until the demo is finished | later |

## Data: what is used, and what was rejected

Checked once, at the time of writing, so re-verify before paying for anything.

| Source | What it gives | Verdict |
|---|---|---|
| **Tardis.dev free samples** | the first day of each month, in the format this repository already parses | **used**: the multi-day evidence grows one free day at a time, with no new code, which is why the two days already here are the first of their months |
| **OKX official historical data** | trade history from 2021-09, funding rates from 2022-03, and high-resolution L2 order book from 2023-03, free and daily | **planned** (phase 2a): the second venue, for consecutive-day tests |
| **IEX HIST, US equity depth** | free DEEP price level and TOPS archives, one pcap per day, T+1, behind a login and the IEX terms of use, and it is IEX's own book only rather than a consolidated one | **planned** (F4): the one market in the table below where a retail commission is smaller than a tick, whose archive needs a DEEP decoder that the published specification has to be obtained for |
| **Databento `mbp-10`** | ten price levels a side with the whole book on every row, so the trades are the `action=T` rows of the same file, billed on the compressed size at half a dollar a gigabyte on the micro E-mini and two on an equity | **used** (F4): the paid route to a tradable market's depth, converted to the CSV pair the existing readers take, with the conversion checked against the vendor's own levels row by row |
| Binance Vision, USD-M futures | free daily aggTrades, trades, book ticker (best bid and offer) updates, premium index klines and open-interest metrics | **used for research outside this repository**: funding and basis work, which needs no price ladder |
| Binance Vision `bookDepth` | percentage bands around the mid, not discrete levels | **rejected**: a tick ladder cannot be rebuilt from it |
| Bybit public data | trades, premium index and spot index; no order book directory | **rejected**: no L2 history |
| Tardis paid, Crypto Lake, LOBSTER, Databento | wider L2 and L3 coverage | **rejected for now**: Tardis has a minimum order far above a few days of data, LOBSTER wants an academic address, and Databento is CME and Nasdaq, which would start a different measurement lineage rather than extend this one |

## What stays inside this repository, and what does not

The demo and a personal trading system share a method and one program, not a codebase. The line is drawn by
what a change produces:

* **Inside**: an input format behind a reader in `src/market/`, a new workload, a new pinned invariant, a
  measured table. This repository accepts **data, configuration and conclusions**.
* **Outside, and a separate project**: strategy research of any kind — designing, fitting and selecting a
  funding, basis or execution strategy, portfolio construction, or scheduling. That work uses this simulator
  as a fee and execution auditor when it needs to know what a trade costs, and it does not add a strategy here
  until the work has become a measurement rather than a search.

The same line explains the rejections above: the sources that would extend the evidence are used, and the
sources that would start a different project are not.

## Venues: what fits a personal user after the demo

Judged as a personal trader rather than as a simulator, and after the gate that comes before every other one: the
venue has to be one you may legally use, and one you can open an account with, where you live. A venue that fails
that gate can still be the best measurement input in the table, which is why access and fitness are separate
columns of reasoning below rather than the same one.

| Venue and instrument | Fit | Why |
|---|---|---|
| Crypto USDT perpetuals (OKX, Binance, Bybit) | **best for measurement** | free consecutive book history with discrete levels, a published fee schedule, and one leg of limit orders is all the model needs, but access is restricted in some jurisdictions, so this is the test bench rather than the trading venue |
| Locally licensed crypto spot venues | possible | legal and openable, but spot only for retail in some regimes, and a fee schedule an order of magnitude above the offshore perpetuals |
| Micro futures on a major exchange | good | accessible through a local broker, a small contract size, and nearly round the clock operation; a quarter point tick on the micro E-mini is worth 1.25 USD against a commission of about sixty cents a contract, so a round turn costs one tick, and Databento sells ten levels of its book for about two dollars a session |
| Equities and ETFs | good | a fee that is a fraction of a tick once the share commission is set against a notional, free daily depth behind a login rather than an endpoint, a free paper account at the broker, and the natural home for a low frequency position rather than a quote |
| Local index futures and cash equities | possible | accessible, but a transaction levy on both sides and paid depth make this the most expensive of the listed markets |
| Derivative products of a licensed crypto venue | only for professional investors in some regimes | the rule is about the account category, not the venue, so it is worth checking before it is planned around |
| Options anywhere | no | expiry, exercise, multiple legs and greeks are not in the model |
| FX and contracts for difference | no | worse data and cost, and the broker is usually the counterparty |

The rule behind the table: a personal account wins on capital and patience and loses on speed, so the venues
that fit are the ones where a position is held rather than won by being first in a queue. Adapting to another
USDT perpetual is a reader and one instrument entry, and the chain of measurements that exists today does not
move, because a second venue is an additional workload rather than a replacement for one.

### Access, cost and data, market by market

The table above ranks fit. This one is the three facts that decide whether a market can be measured at all, asked
for a Hong Kong personal account specifically: may this account trade it, is the fee smaller than a tick, and is
there continuous free depth to test the answer on.

| Market | Tradable by a Hong Kong individual | Fee in ticks | Free continuous depth |
|---|---|---|---|
| US stocks and ETFs | yes, through a broker with US market access | a share commission is a fraction of a cent on a hundred dollar share whose tick is one cent, so about a third to a half of a tick a side, and a zero commission tier makes the fee not the wall at all | **yes: IEX HIST**, DEEP and TOPS, one pcap per day, T+1, free after a login and the terms of use, but it is IEX's own book only, so no national best bid can be built from it |
| HKEX index futures | yes, through local brokers | no stamp duty on futures; the exchange and levy per contract are a fraction of the HK$50 HSI tick and a broker commission is of the same order, so about a tick a side, which is close to the width of a one tick quote | **no**: realtime and historical depth are licensed subscription products, so the only route is to record a broker's own realtime feed into the replay format |
| HK cash equities | yes | a stamp duty of 0.1% a side is about ten basis points, an order of magnitude over any spread measured in this repository, plus levies and commission | no |
| A-shares through northbound | yes | a sell side stamp duty of 0.05% plus commission, and settlement is T+1 | no |
| Micro futures on a US exchange | yes, through a broker | a quarter point tick is 1.25 USD because the micro E-mini multiplier is 5, against a commission of about 0.62 USD a contract a side, so a round turn costs one tick and the fee is half of it a side | **paid and cheap rather than free**: ten levels plus the trades in one file from Databento for about two dollars a session on the micro E-mini, and there is no free archive for it |
| Offshore crypto USDT perpetuals | no: the offshore venues are not licensed here, so this stays a measurement bench | a fraction of a tick at a hundred thousand dollar price, which is the whole problem | yes, and this repository's readers already consume it |
| Locally licensed virtual asset venues | spot yes, derivatives generally professional investors only | above the offshore perpetuals by an order of magnitude | no archive exists, so the same self-recording route as index futures |

Read on 2026-09-30: the IEX market-data page's T+1, pcap, IEX-TP and specification wording, its example file
name, and its Login column; and that IBKR advertises "$0 commissions on US stocks and ETFs with IBKR Lite" on its
own commissions page. Read earlier in this work: the 0.1% Hong Kong stamp duty a side, the 0.05% northbound sell
duty, that HKEX publishes free statistics and announcements but no free depth archive, and that offshore
perpetual depth is free because this repository reads it daily.

Still to verify before anything is planned around it: **whether a Hong Kong resident is eligible for IBKR Lite or
is on the per-share Pro schedule** (the advertised tier is a product fact, not an entitlement), the current list
of SFC licensed virtual asset venues, and the exact HKEX futures exchange fee and levy per contract, since the
contract specification pages this repository tried did not answer.

**The conclusion is narrower than the question.** Only one market in this table has all three columns at once:
a US ETF, priced in cents, with a commission that is a fraction of one, and a free daily depth archive to test
the fee question on. That is why the equity entry is the one being built, and why the local index futures are the
runner up rather than the start: they are tradable and their fee is about a tick, but their depth has to be
recorded before anything can be said about it.

## The carry line: financing flows before carry strategies

Everything measured so far prices one income stream, the spread a fill captures, and one cost, the fee. That is why
every sweep ends in the same place: the spread is a fraction of a tick and the fee is a multiple of it. A second
income stream exists that does not depend on speed or on capturing a spread — a perpetual pays funding, a future
pays or charges the basis at roll, a stock position pays dividends and charges financing — and today the accounting
cannot represent any of it. Adding it is the next capability, and it is deliberately built in this order:

* **First a flow with a fixed rate**, applied per interval to a position, verified by a unit test that states the
  cash, the equity and the realized PnL after N intervals. This makes the mechanism checkable before any data
  plumbing exists, in the same way the queue model was verified before live fills were discussed.
  **Done:** `Portfolio::accrue_interval_flow` and its per-interval pass over the whole book, with those three
  fields asserted after N intervals in `tests/portfolio_test.cpp` for a price-quoted position, a fixed-face
  contract and a short, and the channel described in [Architecture](ARCHITECTURE.md#7-backtest-and-performance-metrics).
  The rate is the per-interval return to a long on the position's notional, so the position's sign decides the
  direction and one rate serves both sides of a market, which is the shape the next step needs.
* **Then a rate series as an input**, because the real rate varies by the hour and changes sign. The free sources are
  the exchange's own funding history, which costs nothing and is a few megabytes per symbol per year.
  **Done:** `tools/fetch_funding_rates.py` fetches either source for one instrument (OKX `module=3`, and the Binance
  Vision monthly `fundingRate` archive, which has no daily folder), `src/market/funding_rate.*` reads both formats and
  the replay applies each settlement as the event clock passes it, reported as a `[FUNDING]` line with the file's own
  arithmetic beside the USD the window moved. Two facts from the real files are recorded with it: the OKX day is cut
  on midnight UTC+8 while its book archive is cut on UTC, and the Binance settlement clock carries milliseconds of
  jitter, so its interval is eight hours to within a second rather than exactly.
* **Then two legs**, because a carry position is a position and its hedge, and the ledger has to hold both without
  letting one leg's exposure be read as the account's.
  **Done:** the `carry` mode holds a stated USD notional on each of two instruments and reports them separately
  and together, because a hedged book holds two notionals and is exposed to none of them. `carry_gross_notional_usd`
  and `carry_net_notional_usd` are both printed so neither can be mistaken for the position, each leg's funding is
  charged at that leg's own mark, and the fills are priced through the same `fee_on_fill` the matching engine uses,
  so a leg booked outside a book cannot cost something else. `carry_breakeven_hours` is the same comparison in the
  form a decision needs, because a fee is paid once while a carry accrues every interval.

The acceptance criterion is one number a reader can check: **annualized carry against every fee the position pays**,
on real data, per venue. **Recorded:** Binance BTCUSDT against OKX BTC-USDT-SWAP in March 2024, 10,000 USD a leg,
728 hours: 304.66 USD of funding paid on one venue and 322.24 received on the other leave 17.58 USD of carry
against 18.00 USD of taker fees, so the annualized ratio is 11.76 while the month itself ends 0.42 USD down, and
the position needed 745 hours to pay for itself rather than the 728 it ran. Held the other way round the same
position loses 35.58 USD with a negative ratio. The criterion's answer is therefore that the line has a number
where the market-making line had none — and that a criterion expressed as an annualized ratio against a one-off
fee can read as a winner on a position that did not quite pay for itself. The two runs and every field of them are
recorded in [Performance History](PERFORMANCE_HISTORY.md#the-acceptance-number) with the command that re-derives
them, and the two funding files are fetched with one command each, named in
[the data index](https://github.com/12mango/ToyQuant-v1/blob/main/data/README.md).

## Back to the market-making line: what would make a quote pay for itself

The carry line answered its criterion and the answer was thin, so the demo's first line is the one worth
returning to — with the question stated as an inequality rather than as a hope:

```text
capture per fill + rebate  >=  maker fee + adverse selection
```

Every term in it is already measured here, and the third was the one no run had reported as a target: the fee is
in the portfolio, the capture and the markout are in `[EXECUTION]`, and the `[PNL]` line now turns the fee into a
rate. The first experiment (`tools/fee_sweep.sh`) asked the fee question directly, on four windows and three
maker rates:

* At a one-tick quote a window loses 0.0030 to 0.0032 USD **before any fee** — about three to four ticks a fill
  against a capture of 0.1 to 0.3 ticks — so its breakeven maker rate is -0.0003, which means the venue would have
  to pay a rebate larger than its own 0.0002 maker fee. Zero maker fees still leave the largest window down 0.86
  USD over 267 fills. **At a tight quote the fee is not the binding constraint; the fill population is.**
* The capture only exceeds that cost at eight ticks of width, where one fill traded and nothing closed: the door
  exists and its width is unknown, which is a statement about sample size rather than about the mechanism.
* Both tables are recorded with every field in
  [Performance History](PERFORMANCE_HISTORY.md#the-maker-fee-each-window-would-have-needed).

The order this leaves, each step measurable on its own:

| Step | What it does | What it settles |
|---|---|---|
| **F1 — the fee question** (done) | `[PNL]` splits quoting from the position left open and reports the breakeven maker rate; `--maker-fee-rate` and `tools/fee_sweep.sh` sweep it | whether a fee tier could pay for this, answered per window |
| **F2 — a decision rule that covers adverse selection** (done) | the gate now requires a quote to cover the fee **and** what our own passive fills at that distance have cost, measured from the venue's prints first and from our own fills after the record showed the two disagree in sign | the mechanism works: refusing the quotes that cannot pay cut the one-tick loss from 0.859 to 0.050 USD and turned the four-tick row positive. What it cannot yet settle is the measured part, because the gate's own success starves its estimator below twenty samples |
| **F3 — inventory discipline** (done) | `--flatten-band=N` reduces a position past N units with a taker order at the touch, and the closed-versus-total split says which part of a result came from quoting and which from the position carried | flattening works and costs more than it saves, and the one positive run in the table was positive *because* it carried a position: the bet paid, not the quoting |
| **F4 — a market where the inequality can hold** (done) | the instrument half is in: `us_equity_spec` states the one cent tick and the commission per share, `micro_es_spec` states the quarter point tick, the multiplier of 5 and the commission per contract, and `fee_on_fill` charges both terms. The data half is Databento `mbp-10` converted to the CSV pair the existing readers take | **the answer is the first positive window in this repository, and it is narrower than the question.** See below |

The three levers a market maker actually has are a rebate, a wide spread and speed. This repository can measure
the first two, model the third, and say which of them a given window needs; what it will not do is present a
window selected for being positive as an edge.

### What F4 found

The same decision layer, the same two hours of one session (2026-09-29 13:30 to 15:30 UTC, the US cash open), on
the two markets where the fee is a fraction of a tick instead of a multiple of one. Every row is re-derived by
`tools/f4_windows.sh` from Databento files that `tools/fetch_databento.py` downloads and converts.

| Window | Fills | Units | Fee USD | Capture, ticks | Markout, ticks | Gross USD | Net USD | Net per fill | Breakeven maker rate |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| micro E-mini, one tick | 1675 | 3934 | 2439.08 | 0.0131 | +0.157 | -387.50 | -2826.58 | -1.6875 | -0.00000255 |
| micro E-mini, two ticks | 57 | 122 | 75.64 | 0.4795 | -0.689 | -196.25 | -271.89 | -4.7700 | -0.00004157 |
| micro E-mini, gate on | 1599 | 3750 | 2325.00 | 0.0111 | +0.160 | -98.75 | -2423.75 | -1.5158 | -0.00000068 |
| micro E-mini, four ticks | 0 | 0 | 0 | 0 | 0 | 0.00 | 0.00 | 0.0000 | 0 |
| **SPY, one tick** | **799** | **33352** | **116.73** | **0.2679** | **+0.092** | **+379.24** | **+262.51** | **+0.3285** | **+0.00001487** |
| SPY, two ticks | 263 | 12279 | 42.98 | 0.6730 | -0.247 | -32.83 | -75.81 | -0.2882 | -0.00000350 |
| SPY, one tick, gate on | 791 | 33116 | 115.91 | 0.2471 | +0.112 | +216.18 | +100.27 | +0.1268 | +0.00000853 |
| SPY, one tick, flattening above 1000 shares | 832 | 34618 | 121.16 | 0.2412 | +0.114 | +282.58 | +161.42 | +0.1940 | +0.00001080 |

What the table says, in the order it has to be read:

* **The fee stopped being the whole story, and on one market it stopped being a loss at all.** A US share pays
  0.35 of a tick in commission against a one cent tick, and at a one tick quote the run ends **+379.24 USD
  before fees and +262.51 USD after them** over two hours, on 799 fills of 33,352 shares. Its breakeven maker
  rate is **+0.0000149**, which is positive: the venue could charge a rate on top of the commission and the
  window would still be flat. Every window measured before this one had a negative breakeven rate, which is the
  statement that no fee tier could have saved it.
* **It is quoting, not the position, that earned it.** The closed part of the same run is **+447.25 USD gross
  and +330.52 after fees**, while the position left open at the end cost 68.01 — the opposite of the earlier
  perpetuals row, which was positive because the carried position paid, not the quoting. The F3 band, which
  exists to stop a run carrying a position, is applied in the last row and *lowers* the result from 262.51 to
  161.42: flattening costs more than it saves, again, and the measure of that is now two markets instead of one.
* **On the micro E-mini the fee is half a tick and the answer is still no, for a different reason.** There the
  commission is 0.62 USD a contract against a tick worth 1.25, so two sides cost about one tick, and the run
  still loses: at the touch the fills capture **0.0131 ticks** each, a hundredth of one, against a fee of half a
  tick. Widening to two ticks multiplies the capture by thirty-seven and cuts the fills from 1,675 to 57, with a
  markout of -0.689: the quotes that fill at two ticks fill because the market is going through them. At four
  ticks, nothing fills in two hours. **The constraint moved from the fee to the fill population and its
  selection, and the fee is no longer what to blame.**
* **The gate that was built for the perpetuals is not a universal improvement.** `--markout-cover` cuts the
  micro E-mini gross loss by three quarters, from -387.50 to -98.75, which is what it was built to do. On SPY,
  whose measured markout is positive, the same gate removes profitable fills and lowers the window from +262.51
  to +100.27. A rule that refuses quotes it cannot pay for is a filter for the case it was built for, and
  applying it to a market that does not have that case costs money.

**Why this is not yet a strategy.** Four things have to be said in the same breath as the positive row. The book
is **Nasdaq's alone**, so the queue ahead of a quote is the displayed size on one venue out of a consolidated
market: a quote that would have been first in the national book can be third here, and the fill population is
therefore optimistic in the one direction that matters. It is **two hours of one session**, in the part of the
day with the most mean reversion. The commission is the tiered rate of a large retail broker and **the tier a
Hong Kong account actually gets is unverified**, and a zero commission tier would add 116.73 to the result. The
model does not include the **venue's own maker rebate**, which on this market is of the same order as the
commission, so the number understates the economics on that side — and it also does not include the regulatory
fees of a few hundredths of a cent a share on the other.


## Deliberately not planned

* **Order-by-order (L3) data.** The bounds experiment shows the conclusions do not move between the
  conservative and the optimistic queue models, so a true queue in between cannot move them either. Finer
  data would buy precision on secondary numbers, at the price of a different instrument and the measured
  history that goes with it. If it is ever worth doing, it is one day of data used to calibrate the existing
  model offline, not a new runtime path.
* **More strategies and more knobs.** The binding constraint is the fee, so a better quote rule changes how
  much is lost rather than whether anything is.
* **Cosmetic repository work.** No demo recordings, no badges. The documents are the interface.

## How the two goals stay compatible

| Choice made now | What it keeps open later |
|---|---|
| every input format behind one reader in `src/market/` | a live feed is another reader, not a second pipeline |
| the strategy talking to interfaces only | the same code can be driven by a replay or by a live book |
| execution reports as the only input to accounting | a real venue's fills update the portfolio unchanged |
| determinism and replay as pinned properties | a recorded live session can be replayed and compared |
