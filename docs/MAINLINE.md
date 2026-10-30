# The main line: the order to read this repository in, and what to drop

This repository is 22,071 lines of source and 27 test binaries, and most of that is not the thing worth learning from.
**The main line is about 5,000 lines**, and it is the path a single market event takes: a file becomes an event, an
event becomes a book, a book becomes a decision, a decision becomes an order, an order becomes a fill, a fill becomes
cash, and cash becomes the number a document quotes. Everything else is either evidence for a closed line, a
compatibility path, or a driver.

This document is a reading order rather than a tour: each step names the file, what to look at inside it, and **the
measured result that step produced**, so that reading the code and reading the evidence are the same activity.

## The line, in one picture

```text
  data file ─► LineReader ─► Pipeline ─► L2OrderBook ─► L2MarketView ─► Strategy
                  │             │                                          │
                  │             │  (trades are the action=T rows)          ▼
                  │             └──────────────────────────► MatchingEngine (queue model)
                  ▼                                                        │
             counts, [DATA] line                                           ▼
                                                            Portfolio (fees, cash, PnL)
                                                                           │
                                                                           ▼
                                                                RunSummary → the documents
```

## Read it in this order

| # | Read | What this step is | The finding it produced |
|---|---|---|---|
| 0 | `src/common/types.h`, `src/common/instrument_spec.h` | What a price, a size and an instrument are: integer ticks with a scale, and a spec that carries the tick, the multiplier, the commission and the four schedule terms | The spec is what makes a one cent tick a **cost** rather than an assumption |
| 1 | `src/market/line_reader.*` | Bytes to fields with no allocation a line: the block reader and the number conversion | The parse was 61 percent of an event and was halved; the integer-plus-scale path is bit-identical to the decimal one |
| 2 | `src/orderbook/tick_ladder.*`, `src/orderbook/l2_orderbook.*` | The book: a ladder sized for one instrument, updated by absolute deltas | Why the ladder is sized rather than a map, measured against the working set |
| 3 | `src/app/pipeline.*` | One event coordinated: filters, the L2 view, the strategy call, the engine call, the summary counters | The filters that stop a trade without a usable BBO from creating a synthetic fill |
| 4 | `src/strategy/strategy.h`, `src/strategy/l2_market_maker_base.h` | The decision, smallest first: quote both sides, refresh on a price move, die on an age | The queue-preservation rule, and why a quote that is not repriced keeps its place |
| 5 | `src/strategy/inventory_aware_l2_market_maker.h`, `src/strategy/active_l2_market_maker.h` | The same decision with inventory, toxicity, a volatility halt and the markout gate | The inventory shift made the ratio worse; the markout gate cut the one-tick loss from 0.859 to 0.050 |
| 6 | `src/exchange/matching_engine.*` | Whether a quote fills: price–time priority, the four queue models, self-trade prevention, partial fills, and the IOC/FOK terminal paths | The bound sweep: the conclusions do not move between the conservative and the optimistic model |
| 7 | `src/exchange/matching_engine.*` (`fee_on_fill`) | What a fill costs: maker and taker rates, a commission a unit, a floor a **order instance**, a fee on a sale's proceeds, a credit for adding liquidity | The floor binds on 79 percent of order instances; a cancel and replace makes a new order |
| 8 | `src/accounting/portfolio.*` | Cash, position, equity, realized and unrealized PnL, and a flow that accrues an interval at a time | The split that made quoting and carrying a position separable numbers |
| 9 | `src/app/run_summary.h`, `src/app/pipeline.*` | What a run reports: the execution line, the order audit, the queue ratios, the markout, the minimum commission paid | Every table in [Performance History](PERFORMANCE_HISTORY.md) is one of these lines |
| 10 | `src/concurrency/spsc_ring.h`, `src/utils/stage_profiler.*` | The two threads and the instrument that measures them | The ring's hand-off counted per event; the profiler's own overhead subtracted from every sample |
| 11 | `src/main.cpp`, `src/app/app_config.*`, `src/app/application.*`, `src/app/strategy_factory.*` | How a run is asked for and wired: the modes, the flags, the presets | Where a flag becomes a behaviour, and why prose is linted against this list |

## What each step is arguing

The table above says what each step **produced**. This one says what it **argues**, which is what is left
after the numbers have been forgotten, and where the argument is made at length. It is the list to walk
when reviewing this repository rather than changing it.

| # | The design idea | Where it is argued |
|---|---|---|
| 0 | a price is an integer tick with a scale, and there is exactly one place it is converted | [Design Review](DESIGN_REVIEW.md), "Prices are integers, and there is exactly one conversion" |
| 1 | a row is a view into a block, and its lifetime is written down rather than assumed | [Design Review](DESIGN_REVIEW.md), "The line reader"; [Low-Latency Design](LATENCY_DESIGN.md), section 1 |
| 2 | the book is indexed by tick, learns its own price window, and answers "is this level there" from the quantity rather than from a bitmap; an update is built aside and swapped in, and the two feed shapes get different checks because only one of them can be crossed | [Low-Latency Design](LATENCY_DESIGN.md), section 2; [Design Review](DESIGN_REVIEW.md), "Zero is nothing" and "The same invariant can be an error on one path" |
| 3 | one class coordinates the event, a metric has exactly one owner, and a trade without a usable top of book cannot become a fill | [Design Review](DESIGN_REVIEW.md), section 1; [Architecture](ARCHITECTURE.md), section 3 |
| 4 | what the pipeline always calls is pure virtual, what only some strategies answer has a default, and a signal is a displacement of one reference price | [Architecture](ARCHITECTURE.md), sections 6 and 6.4.1 |
| 5 | each strategy layer adds one thing, and the queue model is where the fill probability comes from | [Architecture](ARCHITECTURE.md), sections 6.4.2 to 6.4.5 |
| 6 | a queue position is a model and not a price, and delivery order is a correctness property, so the sort is stable rather than fast | [Design Review](DESIGN_REVIEW.md), "Delivery order is a correctness property, not a preference"; [Learning Path](LEARNING.md), section 1 |
| 7 | a fee is a schedule with four terms and a floor per order instance, so a cancel and replace is a new order | [Architecture](ARCHITECTURE.md), section 5 |
| 8 | equity is an identity that holds between marks, and one method accrues every kind of flow | [Architecture](ARCHITECTURE.md), section 7 |
| 9 | every table in the record is one of these printed lines, and state is split by which data it belongs to rather than by how many lines it took | [Architecture](ARCHITECTURE.md), sections 3.4 and 7 |
| 10 | memory ordering is the whole contract, a slot is storage rather than a value, and the instrument that measures the code measures itself first | [Low-Latency Design](LATENCY_DESIGN.md), sections 6 and 4 |
| 11 | configuration is a named struct rather than a positional list, and the flag list is what the prose is checked against | [Design Review](DESIGN_REVIEW.md), "The strategy factory takes a struct, not eleven arguments" and "One place per fact, and the check that guards it" |

## What to drop, and why

The trade-off is not that small is better. It is that **every line a reader passes which is not the lesson costs
attention, and every line that carries evidence has to stay re-derivable.** So the test for each part is whether
removing it would make a claim in the documents unreproducible.

| Part | Lines | Verdict | Why |
|---|---:|---|---|
| `src/lines/legacy/*`, where `l1_market_maker.h` alone is 1,285 | 1,507 | **candidate to delete, with its tests and the tier 1 L1 invariants** | The frozen `v1.0-demo` tag already preserves this line, so the repository does not have to carry it, and nothing on the main line calls it. `OrderBook::on_bbo` is the clearest instance: its only caller left is one test, and the interface it belongs to is still what the legacy tick path reads through `market_top`, so the unit to remove is the line and not the method |
| The legacy CSV and UDP modes and the `Tick` model | ~700 | **candidate to delete** | Unreachable from the L2 path, and kept only for compatibility with a tag that exists |
| `src/market/deribit_readers.*`, `okx_readers.*` and the crypto market code | ~1,500 | **keep, marked as evidence** | The data files are the record of closed lines and one fetch tool still reads the funding history, but this is not reading for the main line |
| `src/market/funding_rate.*`, `src/lines/carry_ledger.*` | ~470 | **keep, marked as evidence** | A marked and checked block in [Performance History](PERFORMANCE_HISTORY.md) re-runs this on every gate: deleting it would either break the gate or turn the sharpest number here into a copied one |
| `src/bench/ring_bench_main.cpp` | 560 | keep | The ring benchmark is quoted in a checked block |
| `src/backtest/*`, `src/app/strategy_benchmark*` | ~900 | keep, off the reading path | Drivers rather than lessons; they are how the tables were produced |
| `src/lines/paper_broker.*`, `src/lines/reconciliation.*` | ~700 | keep, off the reading path | The machinery the roadmap's Stage B needs, exercised by tests |

**The deletions are a separate and gated step rather than a part of this document**, because tier 1 compares the L1
invariants and the legacy replay: removing the legacy line means removing those checks in the same change, and a change
to the gate is a decision rather than a tidy-up. When it is done it is done in this order: delete the code, delete the
tests it owns, narrow tier 1, and run all four tiers before the commit. Until then this table is the record of the
trade-off, and a reader who wants the main line has it above.
