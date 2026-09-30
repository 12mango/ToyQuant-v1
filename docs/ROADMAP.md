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
| Micro futures on a major exchange | good | accessible through a local broker, a small contract size, and nearly round the clock operation; the fee is about a tick per side, which is the same conclusion the simulator reaches for a maker at one tick |
| Equities and ETFs | good | the lowest measured fee of any venue here once the share commission is set against a notional, free history and a free paper account, and the natural home for a low frequency position rather than a quote |
| Local index futures and cash equities | possible | accessible, but a transaction levy on both sides and paid depth make this the most expensive of the listed markets |
| Derivative products of a licensed crypto venue | only for professional investors in some regimes | the rule is about the account category, not the venue, so it is worth checking before it is planned around |
| Options anywhere | no | expiry, exercise, multiple legs and greeks are not in the model |
| FX and contracts for difference | no | worse data and cost, and the broker is usually the counterparty |

The rule behind the table: a personal account wins on capital and patience and loses on speed, so the venues
that fit are the ones where a position is held rather than won by being first in a queue. Adapting to another
USDT perpetual is a reader and one instrument entry, and the chain of measurements that exists today does not
move, because a second venue is an additional workload rather than a replacement for one.

## The carry line: financing flows before carry strategies

Everything measured so far prices one income stream, the spread a fill captures, and one cost, the fee. That is why
every sweep ends in the same place: the spread is a fraction of a tick and the fee is a multiple of it. A second
income stream exists that does not depend on speed or on capturing a spread — a perpetual pays funding, a future
pays or charges the basis at roll, a stock position pays dividends and charges financing — and today the accounting
cannot represent any of it. Adding it is the next capability, and it is deliberately built in this order:

* **First a flow with a fixed rate**, applied per interval to a position, verified by a unit test that states the
  cash, the equity and the realized PnL after N intervals. This makes the mechanism checkable before any data
  plumbing exists, in the same way the queue model was verified before live fills were discussed.
* **Then a rate series as an input**, because the real rate varies by the hour and changes sign. The free sources are
  the exchange's own funding history, which costs nothing and is a few megabytes per symbol per year.
* **Then two legs**, because a carry position is a position and its hedge, and the ledger has to hold both without
  letting one leg's exposure be read as the account's.

The acceptance criterion is one number a reader can check: **annualized carry against every fee the position pays**,
on real data, per venue. If that number is not larger than one, the line has no more to say than the market-making
line did, and it will be recorded that way.

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
