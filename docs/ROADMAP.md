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
| Open | the conclusions rest on one day, and nothing has been run against a live feed; the paced replay and the recorder half of Stage A are in place and a live feed reader is what remains; phase 2b is the plan from the first positive window to a running system |

## The phases

| Phase | Work | What it buys | Cost |
|---|---|---|---|
| **0 — packaging** | one branch, the `v1.0-demo` tag, the documents in the published navigation, a code CI | a reader sees a maintained repository with a history rather than two unfinished ones | done |
| **1 — evidence** | the headline conclusions re-run on a second day, which the repository already holds, and the queue-model bounds experiment recorded | closes the two questions the documents could not answer: does it generalize, and how much does the queue model matter. Both are now recorded, and the second day's numbers are behind a marked block in the gate | done |
| **2 — the live paper milestone** | a websocket book feed, a paper-trading mode reusing the book and the matching engine, and a recorder that writes the live feed in the replay format | the strongest statement available here: one strategy drives a replay and a live session, and a live session becomes replayable | one to two weeks |
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
runs to report the same numbers. What is left of Stage A is the live reader itself.

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
`PaperBroker` (`src/exchange/paper_broker.*`), a decorator over the matching engine that refuses orders,
splits fills, withholds an execution report and takes the venue away for a while, each fault a count of
orders or fills so a faulted run is reproducible from its command line (`--paper-broker` with
`--broker-reject-every=N`, `--broker-partial-every=N`, `--broker-drop-every=N`, and
`--broker-outage-after=N` with `--broker-outage-holds=M`). It keeps its own statement of what the venue
executed, and `reconcile_positions` (`src/accounting/reconciliation.*`) compares that against the portfolio,
reported as a `[RECON]` line: net position and gross traded quantity per symbol, the fills the strategy was
sent against the ones it booked, and fees. Not built, because a paper account is what is missing rather than
the code: the gateway's own transport, idempotent client order ids with a retry path (nothing in this path
retries, so there is nothing for them to protect yet), the hard limits and the kill switch, shadow mode, and
the operational half. Two things the faults found by being run rather than reasoned about: a cancel resolved
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
asserts is exactly the part that cannot be: a time. The feed is still a file reader, so what is left of the
live reader is the transport, and the deliverable still outstanding is the before and after `perf` table for
one optimisation.

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
