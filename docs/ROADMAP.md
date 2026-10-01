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

The screen is therefore **the half spread at the trades, at least three times the fee per side**, measured by
`tools/screen_spread.py` from top of book before any depth is bought; the tool prices a window before it spends
and refuses to download without a stated cap. Both symbols above fail it, which is the expected result for a one
tick market and the reason the search has to leave the most liquid symbol in the world.

### The seven things that must be true before any engineering

| # | Must be true | The cheapest way to kill it | Cost |
|---|---|---|---|
| 1 | the half spread at the trades is at least three times the fee per side | `tools/screen_spread.py`, which prices the window before it spends and refuses to download without a stated cap | 5 to 20 USD |
| 2 | capture stays positive across days once the drift is gone | eight to ten sessions, and the hours outside the open | 10 to 20 USD |
| 3 | our order really rests where we measured it | pin a destination and re-measure, or record where orders actually land for a week and measure that | free, then 1 to 2 USD |
| 4 | the account may run it: day trading minimums, borrow, settlement | a channel answer, not a code change | free |
| 5 | the broker's rate limits and latency carry the pace the strategy needs | its published limits, and the latency the repository already models | free |
| 6 | the strategy survives a realistic round trip | the two latency flags the repository already has, at five and at fifty milliseconds — **run, and it failed** | free |
| 7 | the sign survives the fee tiers | a zero commission tier, a maker rebate, and the worst realistic tier, with the rate at which it flips recorded | 5 to 10 USD |

**A failure on any of the seven sends the quoting line back to a research entry**, and the repository returns to
what it is best at, which is auditing the execution and the fee of a strategy developed elsewhere. That is a
result rather than a setback, and it is the reason these seven come before any live plumbing.

**The sixth gate has now been run, and it returned a failure.** The positive window goes to zero at one
millisecond each way and gives the price away at five, the tolerance is not a personal account's to control, and
the reflex fix of quoting more slowly measures worse rather than better. The rows and the mechanism are in
[Performance History](PERFORMANCE_HISTORY.md#the-latency-that-window-would-have-needed), and the sweep that
reproduces them is `tools/latency_sweep.sh`. Three gates remain, and one of them now carries the whole line: the
screen (1) is what decides whether a symbol wide enough to absorb a delay exists at all. The out-of-sample
campaign (2) and the fee tiers (7) are only worth running on a symbol that screen produces.

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
Three of the five are now in place for the replay path rather than promised: a recording replays to the same
summary, a feed thread hands events to the strategy thread over a ring whose own cost is printed as a
`[LATENCY]` line with percentiles, and the alternatives refused on the way — a dropped event instead of
backpressure, a condition variable instead of a spin, one profiler shared by two threads — are stated where
they are refused. The feed is still a file reader, so what is left of the live reader is the transport, and
the benchmark of the hand-off through the ring is the deliverable still outstanding.

### What has to be done by hand

Everything here is outside a repository: an account, a licence acceptance, a download behind a login, or a
decision about money. It is written down so that code work never waits on a question nobody asked.

* **Channel answers**, before any live plumbing: whether the broker will pin an order to a named destination;
  what a depth subscription costs and at how many levels, remembering that a Level 2 service does not include
  Level 1; whether the account is subject to day trading minimum equity; borrow availability for the short side;
  and the tax treatment of frequent trading.
* **The choice between futures and shares**, which follows from those answers rather than from preference, and
  which decides the symbol screen that everything else is measured on.
* **Downloads behind a login**: one IEX HIST day, if the free replica is wanted at all, which needs a sign in
  and an acceptance of the terms of use. The Databento side is already scripted and needs only the key in the
  environment.
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
