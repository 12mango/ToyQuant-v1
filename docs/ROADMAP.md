# Roadmap

Two goals, one codebase: a demo that shows the engineering, and the option of a personal execution system.
**The demo comes first**, so the phases are ordered by what a reader can see, and each step is chosen so that
it also keeps the later path open instead of paying for it now.

The seam that makes both possible already exists: `Strategy` and `IMatchingEngine` are the only interfaces
the pipeline knows about, and `src/market/` holds one reader per input format. A live feed is another
reader, and paper trading is the existing book and matching engine against that feed.

## Where the project stands

| | |
|---|---|
| Delivered | the queue model, the fee model, the two latency legs, the order trace and the queue distribution, four sweeps of the strategy levers, and a four-tier gate |
| The binding result | at a one-tick spread the fee is 2.57 ticks against 0.088 captured, and no lever moves that ratio |
| Frozen | the pre-queue-model line is tagged `v1.0-demo` |
| Open | the conclusions rest on one day, and nothing has been run against a venue's own live feed; the paced replay, the recorder and the websocket reader are in place, so the seam is closed at the desk and what remains is a credential; phase 2b is the plan from the first positive window to a running system |

## The phases

| Phase | Work | What it buys | Cost |
|---|---|---|---|
| **0 — packaging** | one branch, the `v1.0-demo` tag, the documents in the published navigation, a code CI | a reader sees a maintained repository with a history rather than two unfinished ones | done |
| **1 — evidence** | the headline conclusions re-run on a second day, which the repository already holds, and the queue-model bounds experiment recorded | closes the two questions the documents could not answer: does it generalize, and how much does the queue model matter. Both are now recorded, and the second day's numbers are behind a marked block in the gate | done |
| **2 — the live paper milestone** | a websocket book feed, a paper-trading mode reusing the book and the matching engine, and a recorder that writes the live feed in the replay format | the strongest statement available here: one strategy drives a replay and a live session, and a live session becomes replayable | done: the `ws` mode is the L2 replay path with its two readers moved onto a socket, so the same book, engine, recorder and paper broker run unchanged; the feed speaks RFC 6455 and reads the replay CSV shapes, and a local `tools/websocket_sender.py` closes the loop at the desk — a recording streamed back over the feed reproduces itself byte for byte |
| **2b — from a positive window to a running system** | the plan below, derived backwards from what a live path can be: the deployment filter, the instrument arithmetic, the seven things that must be true before any plumbing, and only then live data, a recorder, an order gateway, risk limits and reconciliation, then minimum size against a real account | a system that is measured rather than asserted, with a stated pass and fail condition at every step | see the stage table below |
| **2a — a second venue** | a reader for the OKX historical format, added to `src/market/` beside the existing ones, plus a reconciliation check that the rebuilt book matches an independent recomputation of the top of book | consecutive-day free data with discrete levels, which is what a multi-day test needs; the existing pinned lineage is untouched because a new venue is an additional workload | done: the readers exist, `market_data_format_of` routes a depth file to the right one, and a paired replay of one hour of the 2026-01-07 archive completes with the summed order book, the exchange's fee schedule and the contract size of 0.01 BTC |
| **3 — system only** | persistence and recovery, the pre-trade gate enabled by default, with a check that the limit exceeds one order, a real venue adapter at small size, multi-instrument accounting | none of it is checkable by a reader, so it waits until the demo is finished | later |

## Data: what is used, and what was rejected

Checked once, at the time of writing, so re-verify before paying for anything.

| Source | What it gives | Verdict |
|---|---|---|
| **Tardis.dev free samples** | the first day of each month, in the format this repository already parses | **used**: the multi-day evidence grows one free day at a time, with no new code, which is why the two days already here are the first of their months |
| **OKX official historical data** | trade history from 2021-09, funding rates from 2022-03, and high-resolution L2 order book from 2023-03, free and daily | **planned** (phase 2a): the second venue, for consecutive-day tests |
| **IEX HIST, US equity depth** | free DEEP and TOPS archives, one pcap per day, T+1, behind a login and the IEX terms of use. DEEP is an **order-level** feed — order adds, executions, deletes and cancels, each carrying an order reference — rather than aggregated price levels, and it is IEX's own book rather than a consolidated one | **rejected**: it is not a venue an account opened from Hong Kong can rest on, so its book can neither rank a symbol (the screen measures where we would trade) nor validate a fill; and turning order-level messages into the price-level ladder this model consumes is new code and a new lineage, against a paid route at about two dollars a session |
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
| US stocks and ETFs | yes, through a broker with US market access | a share commission is a fraction of a cent on a hundred dollar share whose tick is one cent, so about a third to a half of a tick a side; **the zero commission schedule is not available to a Hong Kong account (read 2026-10-02)**, and the per side cost is **not price independent**, because the SEC fee on a sale is charged on proceeds -- see the fee section below | **no usable free depth**: IEX HIST is IEX's own book, so no national best bid can be built from it, and it is an order-level feed for a venue this account cannot rest on. The depth route that is used is paid and cheap: ten levels plus the trades for about two dollars a session |
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

Read on 2026-10-01, from the broker's own published pages rather than from the account, because three questions
that were being treated as account questions are public:
**whether an order can be pinned to a named venue** — yes, and it is a priced routing choice rather than a
switch, because the commission schedule lists "Fixed - Direct Routing" beside "Fixed - IB SmartRouting", so a
directed order is a different schedule on the account that has it;
**depth data** — the market data pricing page sells depth of book per venue, states that a depth service does not
include a top-of-book quote (which has to be subscribed beside it), and waives the monthly fee only above a
stated commission threshold, so a quiet month pays full price;
**the account rules** — the order-status list answers half of the borrow question: an order can come back
`Inactive` because it is a short sale waiting for shares to be located. What the API itself imposes is public
too: a hundred simultaneous top-of-book market data lines by default, market data subscribed per username and
billed the full month rather than pro-rated, and a historical-request pacing that counts fifty simultaneous
requests, sixty requests in ten minutes, six requests for one contract in two seconds, or an identical request
within fifteen seconds as a violation.
**Not confirmed by any page reached from here, and therefore not relied on**: a fifty messages per second client
limit, a forced daily restart of the gateway, and whether a Hong Kong resident is offered the Lite schedule at
all. Those need the account, and none of them changes what can be built next.

Read on 2026-10-02, from the account's own pricing page and from the broker's SmartRouting page, now that an
account exists. **The Lite schedule is not available to a Hong Kong resident**, so the zero commission tier this
document's tables were written against is out, and both remaining schedules are Pro ones: **Fixed at 0.005 USD a
share** and **Tiered at 0.0005 to 0.0035** a share by monthly volume. They are not the same product at two prices.
**Fixed bundles the commission and the exchange fees and still passes the regulatory fees on a sale** — SEC and
FINRA TAF — while **Tiered is fully unbundled**: a low commission, then exchange, clearing and regulatory fees per
trade, and it is the schedule that **passes the exchange's maker rebate through to the account**. Routing is where
they part company. The commission tables list **`Fixed - Direct Routing`** as a schedule of its own, which is the
shape this document read on 2026-10-01; the SmartRouting page says **SmartRouting for US-listed securities is
ordinarily for Pro accounts only** and that it is the facility which can be told to prefer the venue **paying the
highest rebate**, per order or as a global default, so a maker that wants the rebate uses SmartRouting and a maker
that wants to choose the venue uses the Fixed schedule, **and the two may not be available together**. One
exception appears in the same tables -- "tiered - direct routing" on the EUIBSI venue -- so the combination is
decided venue by venue rather than in general, which is why it is a question for the channel and not a fact to
plan on.

What that does to the fee arithmetic is in
[Performance History](PERFORMANCE_HISTORY.md#what-the-accounts-fee-schedule-adds), and it is not small. Reading the
same pages for the terms themselves gives three numbers that matter: the **SEC fee is 0.0000206 of the proceeds of a
sale**, so in ticks it **scales with the share price** -- 0.685 of a tick on a 332 dollar share and 1.58 on a 765
dollar one -- and it is the largest single term in an equity fill; the **Nasdaq credit for adding displayed
liquidity is 0.0013 a share** against 0.0030 for removing it, which is less than half what this document had
assumed a maker rebate was worth; and **every market's schedule carries a minimum per order**, whose note on the
same page says a modified order is charged as a new order. With those three, the four arrangements the account can
choose come out at 0.583, 0.713, 0.863 and 1.113 ticks a fill against the 0.640 an AAPL fill captured, so **the fee
decides whether the quoting covers itself and nothing more** -- and the arrangement that buys venue control is the
worst of the four. All five terms are parameters the engine now prices with, `--fee-schedule=...`, zero unless a run
names them so every recorded row is unchanged, and a run under the account's own figures puts an AAPL fill at
**0.940 ticks against the 0.640 it captured**: on the account's schedule the quoting does not cover itself, by
354.76 USD over the two hours, which the position term of 3,340 still dwarfs.

**The conclusion is narrower than the question.** Only one market in this table has all three columns at once:
a US ETF, priced in cents, with a commission that is a fraction of one, and a free daily depth archive to test
the fee question on. That is why the equity entry is the one being built, and why the local index futures are the
runner up rather than the start: they are tradable and their fee is about a tick, but their depth has to be
recorded before anything can be said about it.

## The carry line: financing flows before carry strategies

**The line is closed and it is kept on purpose, for two reasons.** The recorded run in
[Performance History](PERFORMANCE_HISTORY.md) is a checked block, so the gate re-runs it and compares every value on
every full run: deleting the code would either break that gate or turn the sharpest measurement in this project into a
copied number, and **claims being re-derivable is the property this repository sells**. And it is the only mechanism
here whose income accrues with **time** rather than per action -- an interval flow, a rate series, a two-leg ledger that
reports gross and net notional separately, and a payback measured in hours -- which is what the one variant of this
trade a Hong Kong account can reach and a 50 ms latency cannot touch would need: the **futures basis** on a CME micro,
where the edge is term structure convergence over weeks rather than a venue differential over hours. That variant is
unmeasured, and the existing machinery is what makes measuring it cheap.

### The carry gates executed: the three conditions the owner set, and the answer on nine months

The line's own measurement needed 745 hours to pay for its fees against 728 hours held, and it charged the fee once.
Charging the round trip -- entry and exit, because a differential that flips forces an exit -- makes it 36.00 USD, and
the owner's three gates were then applied to the free funding histories of both venues: a year of Binance monthly
archives against a year of OKX daily archives, both keyless, for nothing.

| month | carry USD on 10,000 a leg | net after the round trip | net annualised on the capital | payback hours | runs of a positive differential | median run | longest run | runs that pay the round trip |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 2024-01 | 26.80 | -9.20 | -0.54% | 999 | 17 | 24 h | 128 h | 0 |
| 2024-02 | 4.34 | -31.66 | -1.99% | 5768 | 16 | 16 h | 72 h | 0 |
| 2024-03 | 19.79 | -16.21 | -0.95% | 1354 | 27 | 8 h | 56 h | 0 |
| 2024-04 | 14.68 | -21.32 | -1.30% | 1765 | 21 | 16 h | 136 h | 0 |
| 2024-05 | 12.94 | -23.06 | -1.36% | 2070 | 18 | 20 h | 104 h | 0 |
| 2024-06 | 39.29 | **+3.29** | **+0.20%** | 660 | 15 | 16 h | 160 h | 0 |
| 2024-07 | 13.44 | -22.56 | -1.33% | 1993 | 15 | 16 h | 104 h | 0 |
| 2024-08 | 3.72 | -32.28 | -1.90% | 7204 | 22 | 16 h | 80 h | 0 |
| 2024-09 | 14.83 | -21.17 | -1.29% | 1748 | 17 | 24 h | 96 h | 0 |
| 2024-10 | 2.43 | -33.57 | -1.98% | 11002 | 18 | 8 h | 72 h | 0 |
| 2024-11 | 63.59 | **+27.59** | **+1.68%** | 408 | 16 | 16 h | 112 h | 0 |
| 2024-12 | 31.69 | -4.31 | -0.25% | 845 | 11 | 16 h | 216 h | 0 |

**No month clears all three gates, and every gate fails by an order of magnitude rather than by a margin.** The best
level is June's 0.20 percent annualised against a gate of ten, and eight of the nine months lose money after the
round trip. The best payback is June's 660 hours against a gate of 72.

**The third gate is the one that explains the other two.** The differential does not persist: a month holds eleven to
twenty-seven separate runs of it being positive, the median run is eight to twenty-four hours, and **not one run in any
month, including the longest of two hundred and sixteen hours, earns enough to pay the 36.00 USD round trip.** That is
the bleeding loop stated as a measurement: enter, pay the fee, watch the sign flip inside a day, leave, and pay it
again.

Two limits on the sample are recorded with the result. All twelve months of 2024 are measured, the three whose first
OKX fetch failed having been re-fetched, and the monthly figures are the two files' own windows rather than the
engine's clock, which is why March reads 19.79 here against the recorded run's 17.58. The fee is the repository's own
measured 18.00 USD a side for two legs, doubled for the exit, and the venue risk, the counterparty risk and the KYC
wall the owner names are not in these numbers at all, because they can only make a failing result worse.


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
fees of a few hundredths of a cent a share on the other. **And the window does not survive latency**: one
millisecond of one-way delay takes it from +262.51 to +1.89 and five milliseconds ends the run holding a position
marked at -1143.75, which is the sixth gate of phase 2b failing and is recorded in
[Performance History](PERFORMANCE_HISTORY.md#the-latency-that-window-would-have-needed).

## From a Positive Window to a Running System

The section above ends at a measurement. This one runs **backwards from the deployment** rather than forwards
from the code, because what a live path can actually be decides the shape of everything else: which markets a
Hong Kong personal account may trade, how many legs that leaves, and whether the fee survives the arithmetic at
the scale a quote really earns it. Each step still states what has to be true, what would kill it, what it costs
and what it leaves behind, so a step that fails is a result rather than a setback.

Two numbers from the recorded row frame the whole chain. About **40% of the positive window's closed profit was
the two legs of the spread** and about **60% was the mid drifting in our favour while the position was held**:
0.267855 ticks of capture per share per fill is about 0.54 ticks on both legs of a round trip, against 1.34
ticks per share realised. And that same run's peak inventory was **1099 shares, about 841164 USD of exposure**.
Drift is not a skill, and the exposure is why size belongs in the claim rather than in a footnote.

### The deployment filter, applied first

| Market | Live for a Hong Kong personal account | What it is left as |
|---|---|---|
| Offshore crypto perpetuals | **no**, not licensed here | the measurement bench, and nothing else |
| Locally licensed crypto venues | spot only; derivatives generally for professional investors | not on the critical path |
| HKEX index futures | yes, but a round turn costs about 0.8 to 1.2 of a tick against a one tick spread | a hold or multi-leg candidate, not a quoting one |
| US stocks and ETFs | yes | **a single leg candidate** |
| CME micro futures | yes | **a single leg candidate** |

The two survivors are one instrument with two-sided quotes, which is what single leg means here: one symbol, one
venue, a bid and an ask resting together, and inventory as the only directional exposure. The account rules then
separate the two, and they separate them in favour of futures:

| Constraint | US shares | CME micro futures |
|---|---|---|
| Day trading minimum equity | applies in a margin account | does not apply |
| Short side | borrow, locate and the short sale price rule | native, no borrow |
| Settlement | T+1, with turnover limits in a cash account | none |
| Margin | Reg T or portfolio margin | a lower intraday requirement |

Everything that table removes is removed from the critical path on purpose: crypto stays a bench, the local index
future stays a hold candidate, the two-legged carry stays a documented capability, and the engineering goes to
one instrument rather than to a portfolio of them.

### The arithmetic that picks the instrument

A fee below one tick is necessary and not sufficient. What decides a market is the capture a fill actually
realises against the fee it pays, and both markets that passed the fee filter have now been measured:

| Market | spread where the trades print | half of it | captured per fill, measured | fee per side | fee / capture |
|---|---:|---:|---:|---:|---:|
| SPY, 2026-09-29 window | 1.000 tick | 0.500 | **0.267855** | 0.350 | **1.31** |
| micro E-mini, same window | 1.000 tick | 0.500 | **0.013091** | 0.496 | **37.9** |

Two things come out of that, and the first one corrected this plan while it was being written. **The spread a
market shows in its book is not the spread its fills happen at**: SPY's median book spread is 2.000 ticks while
the rows where trades print have a median of 1.000, so a screen written against the book would rank symbols by a
number nobody trades at. The second is the gap between the half spread at the trades and what a fill actually
captured — a factor of 0.54 on SPY and 0.026 on the micro E-mini — which is the adverse selection and the timing
inside the fill itself, and which is why neither market pays for its fee even though that fee is a fraction of a
tick.

**Those captures are the conservative queue model, and on SPY that is a floor rather than the window's answer.**
The same window under the other three models fills 2,281 to 2,716 times instead of 799 and captures 0.4189 to
0.4364 ticks instead of 0.267855 — so the model decides the fill count by 3.4x, and it decides the sign as well: a
fill pays one side's commission of 0.35 ticks, so the floor's capture covers 77% of it while the other three cover
120 to 125%. SPY is therefore the first window measured whose **quoting edge sits on the fee rather than under
it**, which is what its positive breakeven rate says from the other side, and it changes what the search has to
find: not a market whose fee is small, but one whose spread is wide enough that a fill's capture clears the fee
even at the model's lower bound. The measurement behind that, the equity book's churn against its traded volume,
and why the effect is larger here than the perpetuals' small fill counts could show are in
[Performance History](PERFORMANCE_HISTORY.md#the-same-measurement-on-an-equity-book).

The screen is therefore **the half spread at the trades, at least three times the fee per side**, measured by
`tools/screen_spread.py` from top of book before any depth is bought; the tool prices a window before it spends
and refuses to download without a stated cap. SPY fails it at **1.43** of the required 3, which is the expected
result for a one tick market, and the sentence that used to end this paragraph said the search therefore had to
leave the most liquid symbol in the world. **The run falsified that sentence.** Thirty symbols over the same two
hours, for **0.52 USD**, and the two that pass both halves of the gate are **AAPL at 4.29 and TSLA at 7.14**,
which are among the most liquid names in the market rather than away from it: on a single venue their displayed
books are three to six cents wide where SPY's is two, and 72% of their trades print against those wider quotes.
The run itself, and what those two symbols then did in the engine, are in
[Performance History](PERFORMANCE_HISTORY.md#the-screen-run-and-the-first-two-symbols-that-pass-it).

### The activity floor, pre-registered

A wide spread with no trades is not a market, and the screen deliberately does not choose this number, so it is
recorded here **before the next screen runs** rather than picked after seeing one. What it is a floor on is the
predicted fill count, because that is what a conclusion needs and the screen cannot measure fills from top of
book. What it can measure is the trade prints, so the question is what a trade print is worth in fills, and the
two windows measured so far answer that:

| Window, 2026-09-29 13:30-15:30 | trade rows | rows the screen read | fills, synchronous | trade rows per fill |
|---|---:|---:|---:|---:|
| SPY | 28,592 | 2,205,840 | 799 | 35.8 |
| micro E-mini | 119,842 | 3,843,413 | 1,675 | 71.5 |

**The floor is 2.8 trade prints a second**, which is 20,000 in a two-hour session. The arithmetic, with each
input named: this repository does not draw a conclusion from fewer than thirty fills
([Design Review](DESIGN_REVIEW.md)); the worse of the two measured yields is one fill per 71.5 trade rows; and
the margin is tenfold, which is the one judgment in the number. The judgment is sized from the F4 sweep's own
result that a quote one tick further from the touch fills about three times less (799 to 263 to none as SPY's
quote widened), taken at two ticks of unknown difference between a candidate's book and these two. Thirty fills
at 71.5 rows per fill is 2,145 trade rows and the margin makes it 21,450, which is written down as 20,000 so
that the floor is a number a reader can hold in their head: 20,000 trade prints in a two-hour session is 2.8 a
second.

Both symbols measured so far pass the floor, at 3.97 and 16.6 trade prints a second, which is the right shape
for it: it is aimed at a market too quiet to measure, not at the two liquid markets this line has already
failed on spread. It is applied by `tools/screen_spread.py --min-trades-per-second=2.8`, which prints
`trades_per_second` and its own verdict beside the spread verdict; the tool does not carry the number, so
changing the floor means editing this paragraph, and doing that *before* the next screen is what makes that
screen's verdict pre-registered.

**It has now been applied, and it is what decided the run recorded above.** The three funds whose spreads are
widest trade far too rarely: JNUG at 43.6 times the fee per side manages 0.016 trade prints a second, NUGT 0.042,
LABU 0.054, against the floor's 2.8, so the floor removes them and their spread is not what disqualified them,
which is exactly the case this paragraph exists for. The two symbols the floor kept trade 7.65 and 6.86 times a
second, about 2.5 times the floor, and SPY comes back at 3.97, which is the number this table derived the floor
from -- the check that the screen and the floor are counting the same thing.

**A second rule is now pre-registered, before any candidate has been screened under it, and it is the first one
with a price in it.** The rule above charges every symbol the same fee per side. That was right while the fee was a
commission and it is wrong now that the account's schedule is recorded: a commission of 0.0035 a share is the same
0.35 of a tick on any share whose tick is a cent, but the **SEC fee is charged on the proceeds of a sale**, so it
is 0.685 of a tick on a 332 dollar share and 0.021 on a 10 dollar one; the account also charges clearing and the
audit trail a share a side, and credits 0.0013 a share for the fills that add liquidity. The round turn in ticks is
therefore a function of the share price rather than a constant:

    round_turn_ticks(price) = 2 * (commission + clearing + audit - rebate + 0.5 * regulatory * price) / tick

with the recorded figures -- commission 0.0035, clearing 0.0002, audit 0.000003, rebate 0.0013, regulatory
0.0000206 -- and the half in front of the regulatory term is because the fee is charged on a sale while a market
maker sells on half its fills. **The rule is the one above with that denominator**: a symbol passes when its median
spread at the trades is at least three times the round turn of its own price. Two consequences are written down
before the run rather than read out of it. The commission-only rule passes a 332 dollar share whose spread is three
ticks; the same share under this rule needs 3.508, so **AAPL and TSLA are expected to fail the rule they passed**,
which is not a reason to change the rule but the arithmetic that makes the price column a search dimension rather
than a footnote. And a ten dollar share with a two cent spread passes here, where the commission-only rule would
have ranked it at 2.86 and failed it. **The minimum per order is deliberately not in the denominator**: it is
charged per order instance rather than per share, so it depends on how often a policy replaces its quotes, which
top of book cannot measure. It stays where it is measured, which is the engine, and a symbol that passes here is
what gets run there. The rule is applied by
`tools/screen_spread.py --fee-schedule=min=0.35,regulatory=0.0000206,clearing=0.0002,audit=0.000003,rebate=0.0013`,
which prints both verdicts, the old one beside the new, so the recorded table stays re-derivable and a reader can
see which rule produced which fail.

**It has since been run, over the thirty symbols already in the cache, for nothing, and it both confirmed and
falsified what was written above.** The confirmation: **AAPL fails the rule it passed**, at a schedule ratio of
2.573 against a threshold of 3.498 ticks, which is the price column doing its job -- a 332 dollar share's SEC fee
is 0.685 of a tick a sale, and it is what pushed the round turn from 0.700 to 1.166. The falsification: **TSLA
passes**, at a schedule ratio of 4.132, because its median spread at the trades is five ticks where AAPL's is three,
and the prediction above said both would fail. The pre-registered arithmetic was also off by a hundredth of a tick:
at AAPL's median trade price of 332.71 the threshold is 3.498, not the 3.508 written above, and the number is
recorded here rather than quietly corrected, because a pre-registration that can be edited after the fact is not
one.

**And the price column did not produce the new candidate it was hoped for.** Every cheap share in the list fails
the spread axis rather than the fee axis: AMC at 3.26 USD, BB at 8.52, TLRY at 4.02 and MSOS at 5.12 all have a
**median spread at the trades of one tick**, which is 0.5 of a tick a side against the 0.35 commission, and the
regulatory saving their price buys them is a fifth of a tick. The US equity tick is a cent whatever the share
costs, so a cheap share is not a wide one in ticks, it is a narrow one, and the names whose spreads are wide in
ticks are the ones the activity floor removes: JNUG, NUGT and LABU pass the spread rule at 38, 24 and 68 times the
round turn and trade 0.02, 0.04 and 0.05 times a second against the floor's 2.8. **The screen has now measured both
ends of that trade-off, and the direction it leaves is a candidate list this cache does not contain**: shares whose
spread is wide in ticks *and* whose flow clears the floor, which on this evidence is a mid-cap single name rather
than an exchange traded fund or a mega cap. That list is the next purchase, it is a few dollars of top of book, and
the rule it is judged against is the one above, written down before any of it was screened.

### The seven things that must be true before any engineering

| # | Must be true | The cheapest way to kill it | Cost |
|---|---|---|---|
| 1 | the half spread at the trades is at least three times the fee per side | `tools/screen_spread.py`, which prices the window before it spends and refuses to download without a stated cap — **run: AAPL and TSLA pass, thirty symbols, 0.52 USD** | 5 to 20 USD |
| 2 | capture stays positive across days once the drift is gone | eight to ten sessions, and the hours outside the open — **its first half is answered for nothing as of 2026-10-02**: the AAPL window cut into eight leaves the capture positive in all eight, and the position term is thirty times the edge, so the second half is unanswerable at a personal account's scale rather than merely unfunded | 10 to 20 USD for full sessions, or **2.30 for ten two-hour windows**, which is the unit every recorded equity row uses — see the budget section below |
| 3 | our order really rests where we measured it | pin a destination and re-measure, or record where orders actually land for a week and measure that | free, then 1 to 2 USD |
| 4 | the account may run it: day trading minimums, borrow, settlement | a channel answer, not a code change | free |
| 5 | the broker's rate limits and latency carry the pace the strategy needs | its published limits, and the latency the repository already models | free |
| 6 | the strategy survives a realistic round trip | the two latency flags the repository already has, at five and at fifty milliseconds — **run, and it failed** | free |
| 7 | the sign survives the fee tiers | **the three this account actually has, read 2026-10-02**: Tiered with the maker rebate passed through, Tiered without it, and Fixed, which bundles the exchange fee and offers `Fixed - Direct Routing`; the zero commission schedule is not available to this account | 5 to 10 USD |

**A failure on any of the seven sends the quoting line back to a research entry**, and the repository returns to
what it is best at, which is auditing the execution and the fee of a strategy developed elsewhere. That is a
result rather than a setback, and it is the reason these seven come before any live plumbing.

**The sixth gate has now been run, and it returned a failure.** The positive window goes to zero at one
millisecond each way and gives the price away at five, the tolerance is not a personal account's to control, and
the reflex fix of quoting more slowly measures worse rather than better. The rows and the mechanism are in
[Performance History](PERFORMANCE_HISTORY.md#the-latency-that-window-would-have-needed), and the sweep that
reproduces them is `tools/latency_sweep.sh`. That sweep's own last paragraph says the one cheap test left is a
symbol whose spread is wide enough to absorb the same delay, and that test is the screen.

**The first gate has now been run too, and it returned a pass.** AAPL and TSLA are the first symbols whose spread
and activity both clear the pre-registered rule, and the engine says what the screen said: their capture per fill
is 0.640 and 0.979 ticks at the conservative model against a fee of 0.35, the first markets in this repository
where the quoting edge covers the commission at every point of the queue model space. **And both windows still lose
money** -- 3,165.53 USD on AAPL and 1,475.53 on TSLA -- for a reason the fee table cannot see: the loss is the
position the run carries, 1,096 and 1,098 shares of it, and tightening the inventory limit lowers the loss
monotonically, because the drift is proportional to the exposure while the capture is not. So the wall moved
rather than fell, and the gate that carries the line is now the second one: does the capture stay positive, with
the position's own move separated out, across days a single session cannot speak for. Gates 3, 4 and 5 are free
and need a channel answer, and the fee tiers (7) are worth 5 to 10 USD now that a symbol exists to run them on.

**The second gate has now been taken as far as it can be taken, for nothing, and it returns a pass and then a dead
end.** Cutting the AAPL window into eight quarter hours needs no new data: `tools/analyze_l2_windows.py` cannot cut
these files, because it re-runs the engine per window and a converted Databento file carries one snapshot, in its
first row, so a slice has no book state to apply its deltas to. The engine already writes every fill it made, so
`out/probe_windows.py` cuts the window from its own ledger instead, and the buckets say two things the purchase
could not have said better. **The capture is positive in all eight quarters**, +0.26 to +1.38 ticks a unit, mean
+0.70, so the first half of the gate passes and the quoting edge is not a one-window artefact. **The position term
swings by 1,930 USD inside one of those quarters**, against a fee bill of 210.58 for the whole two hours, and the
single bucket of the eight that is positive is positive because its drift was +12 rather than because its capture
paid: that bucket's capture, +0.3794, is under the 0.579 the schedule charges a unit before any minimum. That
leaves the expected value: 0.6402 ticks captured against 0.940 a unit charged with the minimum per order and 0.579
without it is **-181 USD a window or +36.70**, so the floor decides the sign and nothing else does -- but a per
window position draw on the order of 3,000 USD against an edge of 37 to 100 makes the sign unobservable, needing
3,600 to 26,000 windows, or fifteen years of daily two-hour sessions at the cheap end. Smaller size does not help,
because both terms are proportional to the volume quoted, which is what makes this a dead end rather than a
funding problem. The rows are in
[Performance History](PERFORMANCE_HISTORY.md#what-the-accounts-fee-schedule-adds).

**The cheap schema was priced as a way around that, and measured against the expensive one instead of assumed.**
The screen's own downloads are top of book only, under the cached files in `data/v2/screen/`, and an AAPL file for
the recorded window was already there, so the comparison cost nothing: converted and run through the same policy it
gives 1,182 fills against the ten level file's 2,324, and 67,157 submitted orders against 45,907. The mechanism is
visible in the same output -- with one level, a book whose touch is taken has nothing left, so quotes land behind
the touch 23,148 times instead of 11,596 -- which is why **the cheap schema is not a substitute for a measurement,
while a two-hour window is the measurement and costs a quarter of a full session**, and the budget below is
corrected accordingly.

### The data budget, and its deadline

One budget fact changes how the three remaining gates should be run rather than whether they can be: the
Databento account holds a **new user credit of 124.03 USD, applicable to historical data only, that expires on
2027-03-30**. Credits are consumed earliest-expiry-first and this is the only one, so there is no second balance
behind it. If the grant was 125 USD, then **about 1.85 USD of it has been spent**, and the components matter more
than the total: 0.97 on the two two-hour `mbp-10` windows the F4 results are measured on, 0.52 on the screen's
thirty symbols of top of book, and 0.36 on the two-hour `mbp-10` pair for the AAPL and TSLA windows -- of which
0.18 was spent twice, because the first pair was downloaded without `--pretty-px`, which leaves the price column
as the vendor's 1e-9 scaled integer and kills the run with `std::bad_alloc`. That is the only money this
repository has spent twice so far, and the flag is now in `tools/f4_windows.sh`'s own comment so the next reader
does not repeat it. The credit is billed on the DBN the query returns rather than on the CSV written, which is why
a re-download of the same window costs the same as the first one.

What it buys, at the rates recorded above (0.5 USD a gigabyte of DBN on the micro E-mini, 2 on an equity, and a
two-hour equity window under a dollar):

**The unit matters more than the rate, and the estimates below were written before that was noticed.** They are
priced for **full sessions of six and a half hours**; every recorded US equity row is a **two-hour window**, a
quarter of a session, and it is the window that a gate is measured on. At the measured 0.23 USD for AAPL's two
hours in ten levels, **ten sessions of two hours cost 2.30 USD and the sixty-odd sessions cost 14**, against the 10
to 20 and 40 to 95 written above. The cheap top-of-book schema is **not** the way to that number -- it was measured
on 2026-10-02 and it moves the fills and the orders by half, so it buys a different measurement rather than the same
one for less -- and the route to a cheaper campaign is the window length and the symbol count, both of which the
recorded rows already use one of.

**The multi-day campaign is now pre-registered, and its rules are written here before it runs.** The purchase is
seven more two-hour windows of MU at 13:30 to 15:30 UTC -- 2026-09-22, 09-23, 09-24, 09-25, 09-28, 09-30 and
10-01, with 09-29 already on disk, so **eight windows in ten levels for 0.608 USD**, priced a day at a time before
any of it was bought. (2026-10-02 is not among them because it is not available: the dataset's own available end is
that day at 13:20 UTC, twenty minutes before the window opens.) MU is the symbol because it is the one whose screen
ratio is 10.770 and whose quoting margin measured positive at matched exposure; AAPL is deliberately not bought,
because the account's own schedule fails it at 2.573.

Each window is run twice, at the pinned size the recorded rows use and at a notional-matched size, both with the
account's schedule. **Four rules decide what the campaign means, and they are stated before anything is measured:**

1. the quoting margin -- capture less markout less every fee including the minimum per order -- is positive in at
   least six of the eight windows: **the quoting edge is out of sample**;
2. the mean and the median of the eight net results are both positive: **the line has an expected value**;
3. the mean is positive and the median is not: **the line is carried by outliers**, it is recorded as that, and the
   exposure cap becomes mandatory rather than optional;
4. the mean is not positive: **the quoting line is closed**, with the numbers that closed it.

What the campaign cannot decide is written down with the rules: eight windows are eight draws of the position term,
which is the term that has decided every window so far, so no rule above speaks to how large the edge is against
that noise -- only to whether the edge is there.

**It has now been run, and it returns a pass on the rule that mattered and a qualified answer on the second.** The
seven windows cost 0.608 USD and the eighth was already on disk: **the quoting margin is positive in all sixteen
rows** -- eight windows at two sizes -- averaging 940.59 a window against a standard deviation of 382.24, so rule 1
holds out of sample. Rule 2 holds at the pinned size, mean +4905.77 and median +4635.13, and falls to rule 3 at the
matched size, mean +743.68 and median -542.82, and **the omission that makes both readings possible is mine**: the
pre-registration above does not say which size the four rules apply to, and it is left in place as written rather
than edited now that the answer is in.

**And the campaign measured the number this line has been carrying as an estimate: the noise is eleven times the
edge.** The quoting margin sums to 7524.69 across the eight windows with a per-window dispersion of 382.24, while
the position term sums to 31721.57 with a dispersion of 10587.96, so **about 506 windows -- two years of daily
two-hour sessions -- is what two standard deviations need** to separate one from the other, and 81 percent of the
campaign's +39246.13 is the position. That is the honest state of the line: the fee question is settled in the
quoting's favour, sixteen rows deep, and the question that remains is not a fee question.

### Where the line goes next, and why the venue is the variable

**The campaign settles the instrument and leaves the venue, and the four properties that decide a venue are written
down as a test rather than as a preference.** A retail quoter needs fees low enough to be a term rather than the
term itself; a spread wide in ticks rather than in basis points; no speed race; and a counterparty that is not a
better informed market maker. US large cap equities fail all four -- a fixed one cent tick, a commission per share
plus a regulatory fee on proceeds, microsecond competition, and Citadel on the other side -- and the eight windows
above are what that failure looks like as arithmetic: 941 USD of margin a window against a 10588 USD position draw.
The venues that pass the same four are where retail market making actually happens, and the repository already has
the plumbing for two of them: **options on a US venue, where the fee is per contract rather than per share and the
spread is a percentage of the premium rather than a tick**, and **crypto venues whose maker fee is zero or a
rebate**, where the readers, the free archives and the carry ledger already exist. Neither is a rewrite; both are
this engine pointed at a different contract, which is what it was built to be able to do.

**The hedge axis has now been measured too, and it says the hedge instrument matters more than the hedge.** On the
one day both instruments exist for, a MES hedge at a notional-matched beta removes **8 percent** of the inventory
term at the one second horizon a fill lives on -- MES explains 9.4 percent of MU's one second variance, against 50.4
percent at five minutes, where the same hedge removes 61.3 percent. So a crude cross product hedge is not a fix for
a quoting strategy, and the reason is worth keeping: **the common factor only appears at horizons longer than a
quote's life.** What that leaves is a screening question the existing tools can answer -- which instrument's returns
explain an inventory's at the horizon the inventory is actually held -- and a same industry candidate, SMH or SOXX
for a memory name and MU options for MU, is the first row of it.

**And the deliverable of this phase is the method rather than the strategy.** What the eight windows bought for
2.96 USD is not a profitable policy but a repeatable one: a pre-registered rule, an instrument priced a day at a
time before it is bought, an engine that separates a quoting margin from a position term and a fee schedule from a
commission, and four gates that re-derive every claim from the binary. Pointing that at a new venue class costs days
and a few dollars, and the record of SPY, of the micro E-mini, of AAPL and of MU is the evidence that it works: four
markets falsified with numbers rather than with opinions, which is the part of this project that is finished.

### The options venue: what has been verified, and the rule that will test it

**The fee census, with its provenance marked because two of its numbers are reported rather than read.** Cboe's own
schedule, read from the page: a **market maker adding liquidity against non-customer flow is credited 0.25 USD a
contract** (code GV), a **customer in XSP, MRUT or DJX trading under ten contracts is credited 0.30** (code XC), a
customer order under 100 contracts routed to certain venues is **free** (code TE), and the pass-through fees for
routed customer orders run **0.18 to 1.25** a contract with the penny classes the expensive ones (RF at 0.75 against
RD at 0.25). **So the sign of a per contract fee is a choice of product and order type rather than a market fact**,
which is the first time this project has had a negative fee inside an account it can actually reach. Reported by the
account holder rather than read from a page, and therefore marked as reported: the broker's commission is **0.65 USD
a contract** at the first tier, **the minimum commission is 1.00 USD an order** whatever the size, and the options
regulatory fee is about **0.02 USD a contract**. The minimum is the one that matters, because it has the same shape
as the per order floor this repository already models for equities, and it makes the clip size a decision rather than
a detail: at one contract the floor spends 1.00 to collect 0.65, and at two it spends nothing extra.

**The rule, pre-registered before any series has been measured.** A series passes when

    half_spread_usd * 100 >= 3 * (commission per order + regulatory + clearing - rebate) / contracts_per_order

evaluated at a stated clip size, because the commission has a per order floor and the rest is per contract. The
multiplier is 100, the tick is 0.05 for a non-penny class, and `tools/screen_spread.py` prices this with
`--fee-schedule=` plus its own multiplier, tick and clip size arguments; and the activity floor was restated rather than
repeated, because 2.8 prints a second is a stock's own tape and the screen below shows no option series reaching it.
Three consequences are written down before the answer is in: the **penny classes fail**
for the reason cheap shares fail, since a one cent spread is 0.50 USD against an 0.87 USD cost; a **five cent spread
is 2.50 USD against the same cost** and passes; and **the fraction of the premium the spread represents does not
matter to the fee**, only to the inventory, which is where the second number below comes from.

**Why this is the venue to test, in one number each.** Capture a unit divided by the notional a unit ties up is
**1.25 percent** here -- 2.50 USD on a 200 USD contract -- against **0.012 percent** for MU, and that quotient is what
decides whether an edge can be separated from a position draw at all; the option's own higher volatility takes some
of it back and the measurement is what would say how much. And the data is a rounding error, priced on 2026-10-02 for
the 2026-09-29 window: the whole SPY chain at one minute sampled top of book **0.2325 USD**, the same chain at one
second 9.32, six near the money series with each trade carrying the quote it printed against **0.00033 USD**, a single
series with the consolidated book **0.00006 USD**, and the chain's own definition free.

**Two risks are written down with the rule, because they are about the data's shape rather than its price.** OPRA
serves no `mbp-1`, so there is no venue specific ten level book for an option: the best available is a consolidated
top of book, which aggregates every venue's size at that price, and this repository has already measured what a
thinner book does to fills, because the top of book experiment halved them. And an option series is one instrument
among thousands on one underlying, so a chain is a many instrument problem for readers that take one file at a time;
a single series is inside the engine's shape, a chain is not, and a delta hedge is a second instrument again, which
the MES measurement says only works at horizons longer than a quote's life.

### Step 1 executed: the SPY chain screen, and the verdict it returns

The screen was run with one argument still missing from `tools/screen_spread.py`, because the ranking that decides the
verdict does not need it; what the clip size moves is the last factor of two and not the sign.

**What was bought, and for how much.** The whole SPY chain at one minute sampled top of book, 0.2325 USD: 203 MB,
1,560,172 rows, **12,626 series** over 119 minutes, with the chain's definition free. Then the trade records for the
40 series of one expiry (2026-10-01) and one strike slab (740 to 779), 0.0853 USD and priced before the purchase,
which are **6,817 prints in two hours**. Total data spend on this venue to date: **0.32 USD**.

**Two numbers came out of the chain before any series was opened.** 8,839 of the 12,626 series are quoted with a mean
spread of at least 0.05 USD in at least 100 of the 119 minutes, so **a wide spread is the rule in this venue** and not
the exception, and the ten widest are 2028 LEAPS with a 4.94 USD mean spread. That is the mirror image of equities,
where one cent of tick held every name to the fee, and it is a real structural difference between the two.

**And then the same measurement that killed JNUG killed the interesting half of it**, because prints and spread run in
opposite directions inside a single expiry.

| series | prints in two hours | prints a second | mean spread, USD |
|---|---:|---:|---:|
| C765, nearest the money | 1,147 | 0.159 | 0.0256 |
| C764 | 510 | 0.071 | 0.0344 |
| C763 | 237 | 0.033 | 0.0560 |
| C762 | 104 | 0.014 | 0.0556 |
| C750, forty strikes away | 28 | 0.004 | 0.1646 |
| C740, sixty strikes away | 3 | 0.0004 | 0.1839 |

**So the rule returns a fail, and on two axes that do not overlap.** The cost clause needs a spread of about 3.5 cents
to clear three times 0.585 USD a contract, and it is met only by series printing four to twenty times too little. The
series with the flow has a 2.56 cent spread, which misses the three times test by about 1.4. The busiest series in the
whole slab manages 1,147 prints, which at seventy-one prints a fill is sixteen fills where the target is thirty, and
**not one series in the slab reaches the restated floor of about 2,130 prints in the window.**

**What is not ruled out, stated before it is needed.** One expiry, one session, one product family. The series screened
are a penny class, so the one cent tick and the 0.75 USD pass-through are both near their worst, and the classes that
carry a rebate were deliberately left unscreened so that the rule cannot be read afterwards as an excuse built to fit
the result. The screen costs 0.32 USD and about a day; the same screen on XSP, which pays 0.30 a contract and quotes
in a different tick, is the one experiment the numbers leave standing.

### Step 2 executed: representative series, and the verdict that replaces the last one

**The verdict above was wrong, and this is the measurement that corrected it.** The screen had looked at one expiry and
one strike slab of SPY and concluded that prints and spread run in opposite directions. Picking the strikes properly
-- nearest the money, on a one day and a thirty day expiry, across four classes -- shows the opposite inside the
nearest expiry: **the series with the tightest spread also has the most flow**, 8,756 prints in two hours at a two cent
spread on an AAPL one day put, and 7,623 prints at four cents on the strike next door. What the first screen measured
was a property of a two day SPY weekly and of forty strikes chosen by hand, not a property of the venue.

**What was spent, and why the second pull cost more than the first.** The free definitions carry every strike and
expiry, so the picks cost nothing; a one day anchor close for the four underlyings cost 0.0000063 USD; the trades for
126 series across two expiries cost **0.899 USD**, three orders of magnitude more than the same call on deep out of the
money strikes, because near the money is where the volume is. Total on this venue: **1.22 USD**.

**The quotient that the screen could not see.** The trades carry the consolidated bid and ask at every print, so the
mid at print times is the contract's own path, and its standard deviation over the same window is the position term
the capture has to be separated from. Against MU's own measurement, 941 USD of capture a window over 10,588 USD of
position standard deviation, or **0.089**:

| series, one day expiry | prints in two hours | median spread, USD | premium, USD | position sigma, USD | capture in two hours, USD | capture / sigma |
|---|---:|---:|---:|---:|---:|---:|
| AAPL P327.5 | 5,276 | 0.02 | 0.57 | 0.091 | 74.3 | **8.20** |
| AAPL P330 | 8,756 | 0.02 | 1.11 | 0.164 | 123.3 | **7.54** |
| AAPL P332.5 | 7,623 | 0.04 | 1.98 | 0.294 | 214.7 | **7.30** |
| AAPL C335 | 7,791 | 0.02 | 1.19 | 0.318 | 109.7 | 3.45 |
| AAPL P335 | 1,668 | 0.15 | 3.22 | 0.533 | 176.2 | 3.31 |

**92 series were measurable, 68 of them at or above MU's ratio, the median 0.206 and the best 8.20**, which is about
ninety times MU before fees and between thirty-five and fifty-seven times after the account's 0.585 USD a contract.
The strike in the third row also clears the other two clauses written down before the answer: four cents is 2.00 USD
against three times the per contract cost, and 7,623 prints is a hundred and seven fills at the ratio the equity screen
used. **So a candidate passes, and the venue is not closed.**

**What now binds, since it is not the spread, the flow or the fee.** The fill ratio of seventy-one prints a fill is an
**equity** number, and at a two or four cent spread both sides sit at the touch: whether a retail quote gets one fill in
seventy-one prints at the touch is a queue question, and the queue is what the engine's own model measures. That is the
next measurement and it is the only one left that can move the answer, because it scales capture directly: a fill in
two hundred prints instead of seventy-one divides the quotient by three, and the margin over the fee goes with it.

**Two risks that this table does not show and the next reader should not forget.** Every winner is a **one day**
expiry, so the premium is small enough that the 1.00 USD order minimum is up to 1.75 times the entire premium, and the
sigma of the position is a lottery's sigma; the edge may be the price of standing in front of expiry flow rather than
market making, which is a different business with a different tail. And the sigma here is measured only at print
times, which understates the risk of holding continuously. Both are recorded before the engine runs rather than after.

| Spend | What it buys | Cost |
|---|---|---|
| the screen (gate 1) | top of book only, over a candidate list and a few days — **spent: 0.52 USD on thirty symbols, AAPL and TSLA pass** | 5 to 20 USD |
| two to three months of daily sessions for the surviving symbol | the out-of-sample campaign (gate 2) at a strength the roadmap's eight to ten sessions cannot reach, sixty-odd sessions instead — **now the second priority rather than the next purchase, because the probe above shows what the campaign would measure is below the noise it would measure it in** | **2.30 USD for ten two-hour windows, 14 for sixty**, and the screen's own row counts narrow it before anything is bought |
| the fee tiers (gate 7) | the same window under a zero commission tier, a rebate and the worst realistic tier | 5 to 10 USD |

**The recommendation is to front-load the evidence, and it has now been followed once.** The other reading of a
credit with a date on it is to hoard it, and that buys nothing: it is 124 USD of historical data that becomes
zero on 2027-03-30, while everything above together is less than the balance. So the screen ran first, for 0.52
USD, because it is what decides whether any of the rest is worth buying, and it produced two symbols. The
out-of-sample campaign's own question has now been answered by a free probe rather than by the campaign: the
capture does stay positive, and it stays positive below the position noise at any size this account can run. **So
the next purchase is one more screen rather than a campaign**, because the pre-registered rule has a price column
it has never been run on -- the SEC fee is charged on the proceeds of a sale, so a cheaper share with the same
spread in ticks pays a smaller regulatory fee for the same capture -- and a candidate list of cheaper, wider names
is a few dollars of top of book, judged against a rule that was written down before any of them was screened. The
campaign follows only if that screen produces a symbol whose capture clears the account's **whole** schedule rather
than its commission; and whatever is left is either spent on more days before the date or written down here as
forfeited, **with the amount**, so that the loss is a recorded decision rather than a quiet one. Two things do not
consume it at all: every replay, sweep and gate run after a download, which is free forever, and the live and paper
phases, whose data comes from our own recorded feed rather than from a vendor.

That also settles the budget rule worth pre-registering: **paid data is only ever bought for the symbol being
validated, one window before a session and one session before a week**. The recurring review that Stage D asks
for is a deliberate purchase each time rather than a standing subscription, and the zero-cost alternative —
recording our own broker's feed — is the default once a live line exists.

### The owner's three decisions, and what each one changes

Three questions were left to the owner, and the answers move work rather than opinions. They are recorded with what
each one changes in a measurement, because a decision that changes no number is a preference.

**The fee floor is per order, and a requote is a new order.** The account holder confirms it directly: partial fills
of the same order on the same trading day are charged once, while a cross-day execution and a cancel and replace each
reset the order instance and are charged their own floor. The repository had already read the broker's own note saying
exactly that, in the Chinese rendering of the page, and had already built the counter that measures how often it binds;
what is new is that the figure is confirmed rather than reported, and that the trap is a stated risk rather than a
footnote. **What it changes is the cost model rather than the market.** The options screen amortised the 1.00 USD floor
over two contracts of one order, which assumes a fill sequence belongs to the order that produced it. A market maker
requotes on every fair price move, so the floor is charged per requote cycle rather than per fill, and the term that
decides a thin margin is the floor divided by the contracts that fill before the quote is replaced. The engine already
reports that count, as filled order **instances** with a size histogram, and the AAPL window already shows the shape:
**1,222 order instances against 2,324 fills, 79 percent of the instances filling fewer than 100 shares.** The queue
measurement is therefore the measurement of two things at once, capture and the number of instances the floor is
charged on, and it remains the only step left that can move the answer.

**The options business is long dated or it is not a business.** The one day expiry series produced the best ratios
measured anywhere in this project and they are set aside deliberately: a one day option's spread is the price of
standing in front of expiry flow, so the capture is compensation for tail risk rather than a quoting edge, the premium
can be smaller than the 1.00 USD floor itself, and the position's sigma is a lottery's. **The pre-registered
restriction is a DTE above fifteen days with moderate liquidity**, which is the part of the options universe that can
be run as a market rather than as an insurance book. Paper trading may still be opened, and its purpose is re-scoped
with the restriction: it is the bench for the order API, the latency and the hedge logic, **not** a test of whether
high frequency quoting pays.

**The equity line is in hibernation, with its reasons measured.** The quoting margin was positive in sixteen of
sixteen windows and the position term drowned it, **10,588 USD of sigma against 941 of capture**, which is five
hundred and six windows to separate them at any size this account can run; the fee floor then binds on seventy-nine
percent of order instances; and no swept parameter moves the ratio by more than twenty percent. Nothing further will
be bought to widen that sample, and the line is closed as a decision rather than left open as a hope.

### Q1 and Q2 executed: the long dated options, and the verdict they return

Two questions were run for nothing, on the trades already on disk, and they close the options line rather than extend
it. The first is the critical fill ratio: with F contracts filled before the quote is replaced, the instance costs
`max(1.00, 0.65 F) + 0.02 F`, so the sign of a fill depends on the spread alone and the size of the window against the
position's own sigma depends on how many prints it takes to get one. That gives one number per series,

    R_max = prints x (half spread - cost a contract) / sigma

the largest number of prints per fill that still covers one window's own standard deviation, and it is comparable to
the 71 prints a fill the equity screen's fill model used.

**The long dated half, which is the only half the restriction allows.** 27 series at or above fifteen days have a
measurable spread and sigma, and **all 27 clear the per contract cost on their spread alone** -- margins from 0.48 to
121.48 USD a contract -- which is the first time in this project that a venue has passed the fee axis. **They then all
fail the other axis**: the largest R_max is **43**, on an AAPL 315 put with 70 prints, and the median is in the single
digits, against a reference of 71. The reason is visible in the print count: these series trade 5 to 264 times in two
hours.

**The line that decides it falls exactly on the expiry the owner excluded.** Across every expiry measured, the series
whose R_max clears 71 are the one and three day ones -- 254, 203, 113, 111, 104 prints per fill allowed -- and every
series at fifteen days or more sits below 43. So the options business that survives the tail risk restriction is
precisely the business that fails the fill ratio, and the business that passes the fill ratio is the one ruled out as
payment for insurance. The equity line's disease is unchanged and it has simply moved: **the fee is no longer what
binds, the number of prints a window contains is.**

**Two caveats, both recorded before the engine could be pointed at a rescue.** The sigma here is sampled at print
times, which understates the risk of holding a position continuously, so the failure is conservative in one direction;
and the bar assumes the position is held across the window, so an inventory path that mean reverts would raise R_max
in proportion. One measurement could still change it and it is the only one left: the engine's fill ratio on a single
long dated series, which would have to come in better than 43 prints a fill where the equity reference is 71. Until
that is run, the line is closed as a recorded decision with its numbers, which is what the owner asked for rather than
another round of tuning.

### The venue picker: what a top of book ratio can and cannot decide

`tools/venue_ratio.py` computes the same quotient on any top of book file the repository already owns, and reports it
as the largest number of prints per fill that still covers one window's own standard deviation -- a fill ratio, and so
comparable to the 71 prints a fill the equity screen used. It was run for nothing over the two things already on disk:
the sixty equity files in the screen cache, and the option series bought for the screens above.

| group | clip | best R_max | reading |
|---|---:|---:|---|
| 60 US equities, whole schedule plus the SEC fee and the maker rebate | 1 share | none, every one negative | 0.35 USD on one share is seventy times the half spread of a one cent market |
| the same | 100 shares | MU 1298, AMD 985, TSLA 872, AAPL 784, CCL 295 | amortising the floor makes every liquid name clear the reference |
| 128 option series, 0.65 USD a contract and a 1.00 USD floor | 1 contract | AAPL P332.5 at 254, then 203, 192, 177, 113 | a contract is large enough a unit that one instance pays its own floor |
| the same | 2 contracts | 344, then 214, 177 | the floor stops binding at two contracts |

**And the table does not settle the venue, which is the finding rather than a disappointment.** The ratio flatters the
wide ticked and heavily traded names -- MU's 1298 prints of room per fill against the options' 254 -- while the engine
measured MU as a loss over eight windows. The two sigmas are different quantities: this one is the standard deviation
of one unit's price, and the engine's 10,588 is the standard deviation of the position the policy actually builds,
which is the price move multiplied by the inventory it carried. **A one lot ratio is a necessary condition and not a
sufficient one, so it cannot rank venues against each other**, and reading it as a ranking would have repeated the
mistake the last three sections each corrected.

**What it does settle is the size of a unit.** A single contract can pay its own floor and a single share cannot, which
is the whole of the options venue's apparent advantage and the reason the one day series stood out. Once the expiry
restriction removes those, the number that decides is the print count a window contains -- 5 to 264 against 7,623 --
which is what Q1 and Q2 already measured.

### Test A executed: the high flow names at 20 to 45 days, which closes the options line

The one corner the expiry restriction allows that had never been measured was the monthly of the names where option
volume lives. It cost 0.0426 USD and it fails: **0 of 41 series clear the reference.**

| series, 2026-10-30 expiry | prints in two hours | median spread | sigma a contract | R_max |
|---|---:|---:|---:|---:|
| TSLA C355, the best of them | 184 | 0.35 | 53.88 | 56 |
| NVDA C230 | 246 | 0.20 | 48.09 | 46 |
| NVDA C235 | 237 | 0.15 | 37.72 | 41 |
| TSLA C350 | 90 | 0.35 | 61.84 | 24 |
| COIN C195 | 6 | 1.63 | 55.48 | 9 |

**The hypothesis was that a high flow name's monthly would print ten to fifty times what a quiet name's monthly does,
and the measurement says it prints the same**: 5 to 246 prints in two hours against 5 to 264 for AAPL, IWM, KO and
XSP, so the flow in these names lives in the one day expiry and nowhere else. Eight classes have now been measured at
twenty to forty five days and the best R_max in any of them is **56** against the reference of 71.

**The five series above were bought again on 2026-10-03 for 0.011938 USD and now sit in `data/v2/options/`**, because
the first copy lived in `/tmp` and nothing behind it could be rebuilt once it was gone. `tools/venue_ratio.py
--input 'data/v2/options/*.csv' --multiplier 100 --fee-per-unit 0.65 --min-commission 1.00 --clip 1,2` reproduces the
prints, the median spreads, the sigmas and the R_max of every row, which is what `python3 tools/viability.py
--derive` compares the recorded 56 against.


**So the options line is closed, and the reason is no longer the fee** -- that axis passed, first time in this project
-- **but the number of times a monthly series trades**, which is a few times a minute at best, against its own price
moving tens of dollars a contract over the same two hours. Spend on this venue to date: **1.27 USD**.

### Line 8 executed: the low turnover rule, and the dispersion that replaces the verdict

The rule was fixed before any window was read, because the point of it is to be falsifiable rather than tuned: a one
minute mid from each file's top of book, a thirty minute simple moving average of it, an entry two ticks from the
average, an exit on the crossing back or after sixty minutes, fills at the touch both ways, and the whole schedule
charged a share on both sides. `tools/lowturnover_test.py` is the test.

**It fails on the median, and the reason is the same arithmetic as the other seven lines rather than a new one.** 57
symbols produced 25 whose net a share a round trip is positive, against a **median net of -0.36 cents a share and a
median gross of +0.40**: the signal is worth about four tenths of a cent a share and the round trip costs about eight.

| | symbol | round trips | mean hold | gross a share | net a share | window net at 100 shares |
|---|---|---:|---:|---:|---:|---:|
| best | AMD | 9 | 8 min | +103.78 c | +101.77 c | +915.89 USD |
| | QQQ | 7 | 7 min | +37.14 c | +34.88 c | +244.16 USD |
| | SPY | 11 | 4 min | +21.36 c | +19.05 c | +209.51 USD |
| worst | LABU | 5 | 14 min | -145.60 c | -146.86 c | -734.30 USD |
| | MU | 4 | 14 min | -92.25 c | -95.21 c | -380.85 USD |
| | MSTR | 3 | 14 min | -28.67 c | -29.73 c | -89.19 USD |

**What this run does not do is settle the rule, and the limitation is the sample rather than the arithmetic.** All 57
symbols are the *same* two hours, so they are 57 independent draws of a statistic whose median is zero and whose values
run from +102 cents a share to -147; 25 positives out of 57 is below half, and with three to eleven round trips a
symbol each figure is dominated by whichever single move its window happened to contain. **A rule like this needs many
windows per symbol, and the only instrument here with more than one is MU**, at eight, and MU is one of the largest
losers in the table. So the line was left unproven pending the replication it named, and that replication has now been run in the same file
by reading the depth shape instead: the same rule over MU's eight windows.

| MU window | round trips | mean hold | gross a share | net a share | window net at 100 shares |
|---|---:|---:|---:|---:|---:|
| 1 | 7 | 12 min | +152.71 c | +149.76 c | +1048.35 USD |
| 2 | 7 | 9 min | +43.29 c | +40.38 c | +282.65 USD |
| 3 | 11 | 7 min | +38.64 c | +35.67 c | +392.35 USD |
| 4 | 6 | 11 min | +35.50 c | +32.52 c | +195.14 USD |
| 5 | 4 | 14 min | -76.25 c | -79.21 c | -316.85 USD |
| 6 | 4 | 18 min | -129.00 c | -131.88 c | -527.51 USD |
| 7 | 4 | 20 min | -261.00 c | -263.95 c | -1055.81 USD |
| 8 | 2 | 40 min | -977.50 c | -980.38 c | -1960.77 USD |

**Four windows are positive and four are negative, so the sign does not replicate**, and the median net is -23.34 cents
a share against a median gross of -20.38. The shape of the failure is worth more than the number: the best window
earned 149.76 cents a share and the worst lost 980.38, so **the rule wins small and loses big**, which is what a
reversion rule does in a market that chops and then trends, because the exit is the crossing back or sixty minutes and
a trend is therefore held for as long as the rule allows. **Line eight is closed**, and unlike the seven before it, it
is closed by payoff asymmetry rather than by a cost, which is why the fix would be a trend filter and why that is a
different claim than the one that was tested.


### The bar every next step has to clear

The direction from the owner is that each iteration answer one question, prove basic profitability first, and only then
ask whether it can be run continuously. Written as a bar rather than an intention:

1. **One question per step**, stated before the step runs, with the measurement named that answers it and the cost
   named before it is spent.
2. **Basic profitability** means, on the measured window, net capture per fill positive after the account's whole
   schedule **including the per order floor**, and capture at least the size of one window's own position standard
   deviation. The first is already printed as fees paid and minimum commission paid; the second is the quotient
   measured above, and MU's 0.089 is the worked example of failing it.
3. **Continuity** means the same sign on at least two non-adjacent windows, which is the check that one window was not
   the market's accident.

The engine core -- market data handling, the order state machine and the gates -- is the asset that survives all of
this, and the venue and instrument decisions above are what it gets pointed at next rather than work to be redone.

### The execution order, once the seven hold

The stages below are the ones a forward reading produces as well, because the shape of a live chain does not
depend on the direction it was derived from; what the filter above changes is which market they run on, and what
it removes from the critical path.

### Stage A — live data and a recorder (one to two weeks)

The repository already says where a live feed fits: the pipeline knows `Strategy`, the engine interface and the
book, and `src/market/` holds one reader per input format, so a live feed is another reader. Concretely:

* a websocket depth-and-trades reader behind the same interface the replay readers use, so the strategy, the
  engine and the portfolio cannot tell a live session from a replayed one;
* a recorder that writes what arrives in the replay shape the readers already take, which is what makes a live
  session replayable and lets the model and the live session be compared on identical input;
* the gate gains an assertion that a recorded file replays to the summary the live run printed, so the live path
  is covered by the same four tiers as everything else.

Two of the three have landed first, because neither needs an account or a data purchase. The delivery half: a
replay can be delivered at the file's own speed or at a multiple of it, and it reports how late each delivery ran
against the schedule the timestamps define, which is the number a live session has to watch. The recorder:
`--record=PREFIX` taps the delivery callback and writes `PREFIX_trades.csv` and `PREFIX_depth.csv` in the shapes
the replay already reads, refusing anything those formats cannot express — a trade with no aggressor side, a batch
whose updates disagree about their local timestamp, a snapshot whose level count changes, or a stream that is both
snapshots and batches at once, which is why the OKX archive cannot be recorded. The gate asserts the claim rather
than describing it: tier 1 of `tools/verify_l2.sh` records the 200k slice, replays the recording, and requires both
runs to report the same numbers. The reader itself has since landed too: `src/market/ws_feed.*` is a websocket
transport behind `IMarketEventReader`, and the `ws` mode is the L2 path with its readers moved onto it. So what
is left of Stage A is not a reader but a **venue** — an adapter that subscribes to one, decodes its messages,
watches the sequence for gaps and reconnects. The route to one, and why it is two steps, is in
[what is left to build by hand](#what-is-left-to-build-by-hand-in-learning-order).

**Passes when** several hours are recorded without a drop and the recorded file reproduces the live summary.
**Ends it when** the depth a retail account may legally subscribe to and read programmatically costs more than
the strategy's gross, or cannot be subscribed at all. That is a licensing question rather than a code one, and
it is one of the two places this chain can stop for reasons outside the repository.

### Stage B — orders, risk and reconciliation (one to two weeks, needs a paper account)

* an order gateway behind the same engine interface, so no strategy code changes: asynchronous submit and
  cancel, partial fills, rejects, timeouts, and idempotent client order ids so a retry cannot double a position;
* a shadow mode that takes live data, runs the strategy and places nothing, recording what it would have done —
  the cheapest way to put the model next to the market for a week;
* hard limits: daily loss, maximum position, maximum working quantity, order rate, duplicate protection, and one
  kill switch that is tested rather than assumed;
* reconciliation of every fill against the broker's positions and cash, which is the check the strategy already
  runs against the engine, extended one layer out;
* operations: one long-lived process, restart recovery that begins by asking what is still working at the
  broker, logs and metrics, alerting, and a written runbook.

**Skeleton, built without an account**: the parts of the four bullets above that a replay can host are in
`PaperBroker` (`src/lines/paper_broker.*`), a decorator over the matching engine that refuses orders,
splits fills, withholds an execution report and takes the venue away for a while, each fault a count of
orders or fills so a faulted run is reproducible from its command line (`--paper-broker` with
`--broker-reject-every=N`, `--broker-partial-every=N`, `--broker-drop-every=N`, and
`--broker-outage-after=N` with `--broker-outage-holds=M`). It keeps its own statement of what the venue
executed, and `reconcile_positions` (`src/lines/reconciliation.*`) compares that against the portfolio,
reported as a `[RECON]` line: net position and gross traded quantity per symbol, the fills the strategy was
sent against the ones it booked, and fees. The client's half of that seam has landed too: `OrderGateway`
(`src/lines/order_gateway.*`) keeps the table of orders from submission to answer, absorbs a retry under the
same client id, and times out an order the venue has not answered, re-sending it under the same id where the
venue's duplicate check makes the retry safe. Not built, because a paper account is what is missing rather
than the code: the transport itself (a socket carrying a venue's own order messages, where the gateway now
models the state rather than the wire), the hard limits and the kill switch as a venue enforces them, shadow
mode, and the operational half. Two things the faults found by being run rather than reasoned about: a cancel resolved
against an order the venue has not been given has to be answered with a `Cancelled` report, or a strategy
that keys on cancel acknowledgements stops quoting altogether — it stopped 51 orders into a 381 order run;
and a fill lost on each side leaves the position exactly where it was, so the reconciliation has to compare
gross traded quantity as well as the net position.

**Passes when** five paper sessions produce no reconciliation break and results inside a tolerance stated
before the stage starts.

### Stage C — minimum size, against real money (two to four weeks)

One share or one contract a side, with a daily table that is the entire point of the stage: live fills, fees,
capture, markout, slippage, reject rate and latency percentiles against what the model predicts for the same
window. Size grows in steps and only while the live column is not worse than the model by more than the
tolerance; three days outside it and the size steps back.

Four account constraints have to be settled first, and none of them is a coding problem: the day-trading and
minimum-equity rules a US equity account is subject to and how they meet a Hong Kong resident's account type,
settlement for a strategy that turns over inventory repeatedly, borrow and locate costs for the short side, and
the tax treatment of frequent trading, which here means no capital gains tax and a business income question
answered case by case.

**Passes when** ten sessions are net positive with live fees and capture inside the model's tolerance.

### Stage D — a measured definition of stable, then scale (months)

Stable is a number or it is a feeling. Over a rolling twenty-session window, all five: net positive with at
least fifteen positive sessions; live fees and capture inside the model tolerance; drawdown under a stated
ceiling; no reconciliation break and at most one outage; availability above 99%.

The loop that keeps it honest: every week the offline pipeline re-runs on the new data and re-derives the
recorded numbers; every quarter the candidate screen runs again, because crowding and spreads move; and any
parameter change needs out-of-sample evidence before it ships. Only then does size grow — same symbol, then
another symbol, then another market. The micro E-mini stays a candidate rather than the plan, because what
failed there was the capture and not the fee.

### What is left to build by hand, in learning order

The stages above are the plan from a positive window to a running system. Read as a learning path instead — no
profit goal, and no need for the statistical rigor the seven gates impose — the same material is a coverage
checklist. The engine already speaks historical data, the L2 book, the queue, the fee, the market-making line,
single-instrument accounting, funding, the paper broker, reconciliation and the recorder. What has not been built
here by hand is the other half of a trading system, and each row points the existing book and engine at a new
workload rather than rewriting them:

| To build by hand | What it teaches |
|---|---|
| the order gateway and the order lifecycle behind it (Stage B) | submit, cancel and modify, partial fills, rejects, timeouts, and idempotent client order ids |
| a strategy that is not market making | signal, then target position, then risk, then exit — landed as `trend_l2`: an EWMA cross, clips at the touch, a stop, a time stop, a cooldown and an exit band, on the shared pre-trade gates |
| live ingestion pointed at a real venue (Stage A) | the venue protocol, subscription, heartbeat, gap detection and reconnect |
| multi-instrument accounting and a portfolio layer | from one instrument to a portfolio and a risk budget |
| persistence and recovery | how a long-lived process keeps its state and asks the venue what is still working |

**How the live reader reaches a venue, in two steps.** The venues whose data this repository already models
publish live data over `wss://`, and the transport here speaks plaintext `ws://` with neither TLS nor a JSON
library in the tree. The first step is therefore a small Python relay, standard library only, that opens the
secure connection, subscribes, decodes the venue's messages into the replay CSV rows this repository already
reads, and re-serves them as plaintext `ws://` on localhost for the C++ feed to read -- which it already can, so
the engine is untouched and the relay is the only thing that knows the venue. It landed as
`tools/venue_relay.py`, standard library only, with three roles that share one decoder: it relays the venue to
the engine, it writes the decoded rows straight to `PREFIX_trades.csv` and `PREFIX_depth.csv` (the direct route,
with no engine), and it captures the venue's raw messages so a window runs again with no network. It subscribes,
decodes, keeps its own sequence accounting -- a break in the venue's `prevSeqId`/`seqId` pair is a lost update
and is counted -- and reconnects with a backoff after a drop, which the snapshot the venue re-sends on subscribe
makes safe for the book. It answers both heartbeat forms a venue can send, a text `ping` and a control ping,
which a six second run never exercises, and it bounds its hand-off to the engine: a full queue waits rather than
growing, and the wait and its high-water mark are printed with the summary. Against OKX's public `BTC-USDT-SWAP` channel a six second run handed the engine 124
trades and 60 book messages, 4807 level rows, with **no sequence gap** and no reconnect; a six second direct
recording wrote 205 trades and 4301 level rows with the same zero gap and replayed through `l2_replay` as a file.
The second step landed too: `venue_relay` (C++, in `src/lines/`) is the same tool with the same flags and the
same rows, so the two are interchangeable in front of the engine. Its decoder was checked against the Python
one on one capture and the two recorded files are byte for byte identical, and a C++ relay serving a capture to
the `ws` engine produced the same event counts. TLS is a build option: with OpenSSL's headers CMake links it and
a `wss://` venue is spoken directly, and without them the C++ relay speaks `ws://` while the Python relay stays
as the TLS terminator, which is why both are kept. OKX is the lowest-friction first target, because its public
channels need no key and its historical endpoint carries no order book.

**A thirty-minute live soak, and the replay that matches it.** The relay served OKX's public
`BTC-USDT-SWAP` channel to the engine, with the paper broker and both recorders on, for 1,800 seconds:
697,400 frames, 6,719 trades and 690,679 depth updates in 17,690 batches, with **0 sequence gaps, 0
reconnects, 0 parse errors and 0 ring waits**, and both processes' RSS flat from the first minute (relay
26.7 to 26.8 MB, engine 7.1 to 7.15 MB). The hand-off queued at most 1,159 rows of its 200,000, so
backpressure never engaged. Replaying the engine's own recording through `l2_replay` reproduced the live
summary field for field -- 323 orders, 6 fills, the same P&L and fees -- and the broker's reconciliation
stayed `status=ok` in both. The heartbeat counters read zero because the stream never went idle for 25
seconds, so that path is verified against a local stand-in that sends both a text ping and a control ping,
not against the venue.

The relay carries two hooks for exactly that: `--ping-seconds=N` sends a ping on a fixed cadence even while
data flows (0 keeps the idle-only keepalive), and `--drop-after-seconds=N` deliberately closes and reopens
the venue connection, so the reconnect path is exercised against the real venue rather than only against a
stand-in. A local stand-in that accepts twice and replies to both pings shows two connections, two pings,
two pongs received and two snapshots, which is the shape the long run has to reproduce against OKX.

**The two-hour run found a deadlock, and the bound that fixes it.** With a forced drop every 25 minutes, the
run stopped at the second drop: the engine's recorded rows froze while the relay kept producing and the
process spun at one core. The cause is the merge between the two blocking readers. It waits for the next
trade while depth keeps arriving, so a trade gap longer than the depth ring fills the ring and the receive
thread blocks on a push the merge cannot drain. The numbers line up exactly: 384 depth rows a second against
4,096 slots is 10.7 seconds, and the longest trade gap measured here is 10.2 seconds in thirty minutes and
15.7 seconds in the longer recording. The live input rings are now 131,072 rows -- about 5.7 minutes of
depth -- and the feed reports `ring_high_water`, so the margin is a measurement. The live feed now delivers
one ordered stream: the receive thread assembles a depth run, holds a trade that arrived during it, and pushes
both in order, so there is no second blocking reader for a trade gap to stall. The input ring holds 131,072
rows as a margin and reports its high-water; over two hours that was 3,917 against the old 4,096, which is why
the two-reader version could stop exactly there.

The rerun completed the two hours: 5,704,944 frames, 144,755 trades and 5,560,187 depth updates, with
**four forced reconnects and zero sequence gaps**, 355 pings answered by 355 pongs, `ring_waits=0`, and a
ring high-water of **3,917 rows** -- 179 below the old 4,096 capacity, which is the measurement that says
the old bound was not a theoretical worry. `incremental_snapshot_batches=5` says the book resynced on the
initial snapshot and on each of the four reconnects, and replaying the engine's own recording reproduced the
live summary field for field. One strategy edge is visible in the same run: `active_l2` shrank some quotes
to 5,000 units, below the instrument's 10,000-unit minimum, so the engine refused them; the reconciliation
stayed `ok` and the network result is unaffected, but a clean strategy run should set `--order-size` at or
above the minimum.

**Why UDP is not part of this yet.** The only UDP in the tree is the legacy receiver in
`src/lines/legacy/udp_feed.*`, and it is a receiver for the frozen `Tick` line: one CSV row per datagram, no
sequence, no gap detection, no recovery, and a datagram dropped silently when its ring is full. A live
`MarketEvent` feed over UDP is a different thing -- an exchange-native multicast feed needs a licensed or
colocated source, a sequenced packet header, A/B line arbitration, and a second channel that fills a gap from
a snapshot or a TCP replay. This demo has none of those, and inventing them against a public venue would
model something the account cannot legally receive. So the two representative chains stay on the transports
they can actually get: `wss://` for the push chain and files for the pull chain. A UDP chain, if it is ever
wanted, is a third, simulated one with its own header, gap detection and recovery, not a promotion of the
legacy receiver.

**The two chains, and the order of the next iterations.** The push chain is a venue's public stream, decoded
by the relay and read by `WebSocketFeed`; the pull chain is a day file read by the venue readers. Both end at
the same book, strategy, engine and portfolio, and the recorder is the bridge: a push session written back to
a replay CSV and replayed by the pull chain has to report the same numbers. The cheapest order is to close
that loop before broadening the venue list, because it reuses files already on disk and code already written.

The pull chain needs no phase of its own. It is the historical path the gate and the eight closed markets
already run on, and the recorder already proves the two chains meet: a push session written back to a replay
CSV and replayed by `l2_replay` reports the same numbers. What is left is new capability rather than more
proof, so the order below is by cost and by how much of the system each one adds.

1. **A second venue decoder.** Binance's public streams (`@depth`, `@aggTrade`) are a different JSON shape
   and a different sequence pair (`U`/`u`), which is the cheapest way to prove the decoder seam is real
   rather than an OKX-shaped special case.
2. **Multi-instrument, the one structural change.** The relay's instrument argument becomes a list on a single
   connection -- one socket and one subscription, because a connection per symbol is what rate limits punish
   -- and the engine grows from one `L2OrderBook` to one per symbol. The book is the small part: the position,
   the strategy state, the matching engine's queue bookkeeping and the portfolio are per-symbol too, so this
   is a refactor of the pipeline rather than a map in the feed. Rows route by their `symbol` column (the
   second, not the first), and an instrument with no `InstrumentSpec` still refuses the run. It is worth doing
   only when a cross-instrument use case -- a hedge, an arb -- is what the demo is for; without one it is a
   refactor that buys no new behaviour.
3. **The simulated UDP chain, only if it is wanted.** A `MarketEvent` feed with a sequenced header, gap
   detection and a snapshot recovery hook, plus a local sender that drops and reorders datagrams so the
   recovery is exercised. It teaches loss, ordering and backpressure; it is not a route any account here can
   trade on.

If the next goal is a fuller trading system rather than more ingestion, the order gateway and persistence and
recovery in the table above come before multi-instrument: they add a part of the system that no file or
socket already exercises.

**Not planned, and why.** Kernel bypass (`DPDK`, `SO_BUSY_POLL`), `O_DIRECT` and DTLS are production answers
to a licensed multicast feed this demo does not have; the recorder's buffered stream and the relay's
background socket are enough for the volumes a public stream produces. If a capture ever drops a row, the
first fix is a writer thread behind a queue, not direct I/O.

**A production checklist, sorted by this repository's measurements.** Five suggestions came from outside:
shared-memory IPC, SIMD parsing, RCU configuration, a template pre-trade risk pipeline, and binary telemetry.
Each is a real technique, but only two have a reason in this repository's own record -- a missing control and a
measured bottleneck; the other three are recorded with the condition that would make them worth doing, so the
list does not become a checklist that drives work by itself.

1. **A pre-trade risk pipeline is landed.** The kill switch, the exposure cap and the position gate moved out
   of `Pipeline::submit_strategy_actions` into `src/app/risk_pipeline.*`, an ordered list of five checks, and
   the two missing controls are in it: a fat-finger band around the midpoint and a token bucket on
   submissions. `tests/risk_pipeline_test.cpp` covers each check and the precedence, and
   `tools/risk_check.sh` measures four runs: the no-flag run must reproduce the baseline, a permissive
   setting must leave `[EXECUTION]` unchanged, and each named control must refuse orders and still reconcile.
   The first measurement found the next problem: a rejected order is retried on the next view, so a 0.01
   orders-a-second bucket produced 33,685 refusals for four submissions. The next iteration is a strategy-side
   attempt backoff, so a refusal is not retried on every view.
2. **A block/SIMD parse is worth one bounded experiment, on the replay path.** `read+parse` is 61% of an
   event and the row loop is at its floor; the one shape change left in the performance record is parsing a
   block instead of a row. `simdjson` does not apply -- the wire format is CSV -- so the experiment is a
   delimiter scan plus the existing fixed-point conversion, measured on the 2M-row slice with
   `--profile-stages`, byte-identical output, and rejected if the `read+parse` p50 does not move by more than
   10%. It does nothing for a live session at 800 rows a second.
3. **Configuration hot update, when live tuning exists.** The direction is right -- an immutable parameter
   snapshot behind an atomic pointer, or a seqlock -- but a bare `std::atomic<Config*>` swap is a
   use-after-free waiting for a reader that still holds the old pointer. The safe minimal form is a snapshot
   the reader takes once per decision and the writer replaces whole; full RCU needs epochs or hazard pointers
   and is more machinery than a single strategy thread justifies.
4. **Shared-memory IPC is deferred.** The live path carries about 800 frames a second, an event every 1.3 ms,
   and the decision is a small part of that; local TCP's tens of microseconds is not the limit. The cheaper
   step first is the C++ relay with OpenSSL, which removes Python and the local hop
   entirely. A shared-memory ring would also have to carry a fixed-size POD, not `MarketEvent` with its
   strings and variant, and a Python writer with the right release/acquire ordering; the debug and replay
   formats would both grow a decoder.
5. **Binary telemetry is deferred.** The two-hour run wrote 10,673 orders and 142 fills -- 1.5 orders a
   second -- so CSV is not on a hot path, and it is part of the verification story: the gate counts rows and
   the documents quote the fields. Revisit above a few thousand orders a second, with an SPSC queue to a
   writer thread and a binary format, and keep CSV as the audit format rather than replacing it.

### The same code, second purpose: the demo and the interview

Everything below is live-path work or a measurement of it, so the demo and the deployment share one codebase
rather than paying for two. The repository's existing strengths are already interview material — integer prices,
one parse per row, a jump table over columns, four updates inline before any allocation, four queue models with
a calibration behind them, and a four-tier gate whose reference hash has to be justified when it moves. The list
is what a live path adds on top.

| Iteration | C++ and low-latency surface | What it demonstrates |
|---|---|---|
| the live reader and the recorder | epoll or a busy poll socket, `std::span`, zero copy parsing, and the same parser the replay path uses | one strategy driving a replay and a live session, with the live session replayable |
| handing events between threads | a single producer lock free queue, `std::atomic` memory ordering, `alignas` against false sharing, batched wakeups | the classic low-latency question, and a live requirement rather than a decoration |
| latency reporting | `std::chrono::steady_clock`, histograms, percentiles per stage from feed to decision to order | low latency as a number rather than an adjective |
| optimisation evidence | `perf stat` instruction and cache counters, the existing libc call counter, and the documented rejection of `std::pmr` | a choice with a stated alternative beats a feature list |
| the gate itself | four tiers, a re-derived reference hash, a prose lint, and sanitizers | a repository where a documented number cannot drift |

Five small deliverables come out of it: a paper session whose recording replays to the same summary; a
feed-to-order latency table with percentiles; a benchmark of the inter-thread queue; a before and after `perf`
table for one optimisation; and a page of stated alternatives, in the style the design review already uses.
Four of the five are now in place for the replay path rather than promised: a recording replays to the same
summary; a feed thread hands events to the strategy thread over a ring whose own cost is printed as a
`[LATENCY]` line with percentiles; that hand-off is benchmarked in isolation by
`src/bench/ring_bench_main.cpp`; and the alternatives refused on the way — a dropped event instead of
backpressure, a condition variable instead of a spin, one profiler shared by two threads, an 8 slot ring
instead of 4096 — are stated where they are refused.

The benchmark measured the hand-off at 18.3 ns/message for a trivial payload and 169.9 for the event this path
carries, 1.8x the mutex and condition variable alternative, with the event's own move and destructor as the
dominant term. The table is in
[the performance reference](PERFORMANCE.md#39-the-hand-off-between-two-threads), and the part of it no gate
asserts is exactly the part that cannot be: a time. The transport has since landed as `src/market/ws_feed.*`, so
what is left of the live reader is the venue adapter rather than the pipe, and the deliverable still outstanding
is the before and after `perf` table for one optimisation.

### What has to be done by hand

Everything here is outside a repository: an account, a licence acceptance, a download behind a login, or a
decision about money. It is written down so that code work never waits on a question nobody asked.

* **Channel answers**, before any live plumbing: whether the broker will pin an order to a named destination;
  what a depth subscription costs and at how many levels, remembering that a Level 2 service does not include
  Level 1; whether the account is subject to day trading minimum equity; borrow availability for the short side;
  and the tax treatment of frequent trading.
* **The choice between futures and shares**, which follows from those answers rather than from preference, and
  which decides the symbol screen that everything else is measured on.
* **Downloads behind a login**: none, now that the IEX route is rejected for the reasons in the data table above;
  the Databento side is already scripted and needs only the key in the environment and a cap.
* **Money and a paper account**: a funded account for the minimum size stage and a paper account for the
  reconciliation stage.
* **The two decisions only the owner can make**: how much capital, and whether the offshore venues stay a
  measurement bench or are dropped entirely.

### What would stop the chain, in the order it is likely to

| Breakpoint | Why it sits where it does |
|---|---|
| **One venue's book is not the market** | gate 3 measures it, and it is the largest single unknown in the positive row |
| **Retail latency** | Already measured on a perpetual: twenty milliseconds of cancel latency turned 0.095 ticks of capture into -0.44. If a US book behaves the same way, the quoting line is not a personal account's game |
| **Account rules** | Day-trading minimums, settlement and borrow decide whether the strategy may be run at all, before any engineering |
| **Depth data licensing** | Decides whether the live path can exist, and the repository cannot answer it |
| **Capacity** | The positive window made 262.508 USD in two hours on about 150000 USD of quotes and a peak of 841164 USD of exposure. Twenty to forty times that size is what an income would take, and at that size the fill model is gone |

### What this plan does not claim

The costs and probabilities below are the planner's stated priors, not measurements, and they are written down
so the plan can be argued with rather than trusted. The rule the rest of this document follows applies here too:
a window that is positive is not an edge, and a plan that ends in real money is worth nothing until its first
stage has tried to kill its reason for existing.

| Stage | Calendar | Cost | Prior of passing |
|---|---|---|---|
| the instrument screen, offline | two to three days | 20 to 50 USD | a symbol whose half spread is three times its fee is found about three times in five |
| the seven gates, offline | one to two days | under 40 USD of data, plus a few channel answers | gate 6 has already failed on the window it was aimed at, so the rest now decide whether anything is left of the line |
| Stage A, live data and recorder | one to two weeks | 0 to 30 USD a month | engineering succeeds about nine times in ten; the licensing question is the risk |
| Stage B, orders, risk and reconciliation | one to two weeks | a paper account | engineering succeeds about four times in five |
| Stage C, minimum size | two to four weeks | capital, and the account rules decide how much | live reproduces the offline result roughly a third to a half of the time |
| Stage D, stable then scale | three to six months | ongoing | the quoting line reaching stable positive: about one in six; the slow line, which does not depend on speed, is the better bet |


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
