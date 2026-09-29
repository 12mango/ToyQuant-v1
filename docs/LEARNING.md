# Learning Path

This document is the market-making side of the project: the mechanisms the simulator exists to show, in the
order they matter, each with the measurement that supports it. It is short on purpose and it points at two
places for depth. [Design Review](DESIGN_REVIEW.md) explains why the simulator is built the way it is and
lists what the evidence cannot yet support; [Performance History](PERFORMANCE_HISTORY.md) holds the
measurements themselves.

Sections 1, 6 and 7 are one thread and are best read together: section 1 claims that a fill is a queue
statement rather than a price statement, section 6 shows that claim as a single order's trace, and section 7
shows what changes when the queue is harder to leave than to reach.

This is a market-making simulator for learning, not a profitability claim. It exists to make four
mechanisms visible and measurable, and this document walks them in the order they start to matter. Each
section says what to run, which field answers it, and what the recorded numbers look like, so a reader
can tell a correct result from a surprising one.

Everything below uses the recorded Deribit window. Cut the slices first:

```bash
bash tools/verify_l2.sh fast        # also checks that the code still reproduces its recorded numbers
```

## 1. Fill probability is a queue, not a price

An order fills when the trades at its price have consumed everything ahead of it. The strategy's own
size is almost irrelevant: a ten-contract quote is 0.05% of the median level, so what decides a fill is
where the queue went.

```bash
tools/workload_matrix.sh model       # fills under each queue model, same window
tools/workload_matrix.sh placement   # fills as the quote moves from behind the touch onto it
tools/workload_matrix.sh requote     # fills as the quote is held instead of re-joined
tools/workload_matrix.sh arrival     # fills as the assumed arrival position changes
```

Read `trade_reports` (fills) and `quote_at_touch_orders`. The measured ordering of the levers, largest
first: **requote threshold 7.7x, queue model 5x, quote placement 3x, arrival share under 2x**. That
ordering is the single most useful thing on this page: a reader who wants more fills should change the
requote policy, and a reader who wants a better model should look at whether cancellations help clear
the queue, not at where the order arrived.

The arithmetic is in `docs/PERFORMANCE_HISTORY.md`, under "The queue model, as arithmetic". The short
version: a level decrease of `r` has one unknown, how much of it was ahead of us, and the models are
claims about that number's mean, with `--queue-chunks` controlling its variance and `--arrival-share`
its starting point.

## 2. Adverse selection is what happens after the fill

A maker earns the spread at the fill and gives some of it back when the market keeps moving.

Read `captured_edge_per_unit_ticks` (what the fill was worth in ticks) against
`markout_per_unit_ticks` (what it was worth five quote cycles later). On the recorded window the first
is 0.4 to 2.5 ticks and the second is -0.5 for the fills that resolve, so a large part of the spread is
given back. `markout_count` says how many fills were old enough to be resolved, which matters because a
markout that never resolves is not evidence of anything.

## 3. Fees decide whether the spread was worth earning

One BTC-PERPETUAL contract has a fixed 10 USD face value at a price near 6,421, so a half-dollar tick
is worth `10 * 0.5 / 6421 = 0.0008` USD, while a 0.02% maker fee on the same contract is `0.002` USD.
Covering the fee takes about 2.5 ticks of capture per side; a quote at the touch earns 0.4 to 0.7.

Read `captured_edge_usd` against `fees_paid` in the summary. More fills raise the fees and barely move
the edge, so `realized_pnl` gets worse as the fill count rises. This is the honest result of the demo:
on this instrument and this fee, a passive quote at the touch cannot pay for itself, and no queue model
changes that.

## 4. Inventory is the risk that survives all of the above

Read `max_abs_inventory`, `inventory_sign_changes`, `avg_abs_inventory`, and in `[PORTFOLIO]`
`unrealized_pnl`, which is the open position marked at the last market price. `max_abs_exposure_usd`
against `starting_cash_usd` says whether the run ever held more than its collateral allowed, and
`exposure_over_collateral_fills` counts how often a fill pushed it past. `--max-position=N` turns that
observation into a pre-trade gate. `strategy_position_mismatches` compares the strategy's own position
against the pipeline's, and a non-zero value there means a callback was dropped.

## 5. Reading a comparison without fooling yourself

```bash
tools/compare_runs.sh "conservative" "l2_replay ... 1 conservative --fast-validation" \
                      "prorata"      "l2_replay ... 1 prorata --fast-validation"
```

- **Fewer than 30 fills supports no conclusion** about PnL or per-fill edge. Every recorded window is
  far below that, which is why this project reports a range across queue models instead of a number.
  The benchmark marks each row with `fills_are_conclusive` and warns on stderr.
- **Steps do not add.** Two changes that each give -6% are not a -12% change; interactions are real.
- **A simulation is not a measurement below its own noise.** Timing claims have the same rule, recorded
  in `docs/PERFORMANCE.md`.

## 6. Seeing the queue instead of trusting the statistic

Section 1 says a fill depends on the queue in front of a quote. That is easy to assert and hard to believe,
because every fill rate the project prints is a total: 182 orders, 21 fills, 11.5%. `--trace-order=N`
replaces the total with one order's sequence. Below is the complete trace of order 3 in the window the
performance tables use, at the policy with the best measured capture, with the timestamps quoted as
milliseconds after the order was sent and the rows narrowed to the two columns that matter. The submit row
has no queue to report and prints zero, because the order has not joined a level yet: the field belongs to a
resting order's wait, not to the price.

```
event   queue_ahead   change
submit  -
rest    7200                      <- the queue the order joined behind
view    7199          1
view    2135          5064        <- one cancellation ahead removed 70% of the queue
view    2134          1
view    2063          71
view    1995          68
view    1929          66
view    1865          64
view    1803          62
view    1743          60
view    1723          20
view    1704          19
view    588           1116        <- a second lump, the last big one
view    569           19
view    507           62
view    391           116
view    304           87
view    236           68
view    211           25
view    189           22
view    186           3
view    169           17
cancel  0                         <- pulled with 169 contracts still standing in front
```

Four things are in there that no total can show.

**The order waited behind 7,200 contracts to trade one.** A new quote does not get to cut in: the model
makes it join behind everything the venue was already displaying at that price, which for a Deribit perp
level is thousands of contracts.

**The queue does not erode, it collapses.** 86% of everything that left went in two events, 5,064 at the
first view and 1,116 later on. The remaining twenty-one views moved it by tens. A model that spread the
same total reduction evenly over the same events would reproduce the average and get the distribution
wrong, and it is the distribution that decides who fills first.

**The quote was pulled while 169 contracts still stood in front of it.** The strategy's own price rule
moved and it cancelled, so the queue never reached zero and no fill was possible. That is the mechanical
reading of the fill rate: 21 fills from 182 orders is less a statement about prices than about how long a
queue takes to clear and how quickly the strategy changes its mind.

**The first order of the run did not last a millisecond.** Order 1 joined behind 208,300 contracts and was
cancelled in the same millisecond it was submitted, which is what quoting one tick wide at a touch one tick
away looks like when the strategy re-quotes immediately.

Two notes on building this, because both are the kind of thing that hides inside a plausible number. The
first version printed the cancel at the timestamp the order had been submitted: an order's own reports
carry the order's timestamp rather than the event's, so the sequence read backwards from its second row.
The trace now times every row with the clock of the event being processed. And `--no-output` silences the
summary, so the first traced run that used it printed nothing at all; the trace rides in the summary and is
meant to be read with `grep QUEUE_TRACE`.

The trace is one order; the distribution is all of them, and it is the version that carries the claim.
`queue_profile_orders`, `queue_zero_orders` and the two `queue_..._fraction_p50` fields in the summary are
the same measurement aggregated over every quote the strategy posted, and on the window above the median
quote never came closer than 96.1% of the queue it joined behind. Section 1 of
[Design Review](DESIGN_REVIEW.md) is the claim and section 4 is the evidence behind it.

## 7. Latency is two costs with opposite signs

The natural way to model latency is a single number that makes everything slightly worse. Measured on this
feed it is two numbers that pull in opposite directions, and they are not the same kind of cost.

| added latency | fills | captured ticks per contract | realized USD |
|---|---:|---:|---:|
| none | 21 | +0.095 | -0.109 |
| 20 ms to arrive | 11 | +4.09 | -0.016 |
| 20 ms to cancel | 25 | -0.440 | -0.156 |

**A late arrival quotes wider.** The strategy chooses a price at one view and that price reaches the book
milliseconds later, by which time the touch has usually moved. The order ends up where the strategy would no
longer put it, which on this feed means further from the mid, and further from the mid is a better price for
the side that gets filled. Fewer orders trade and each one that does is worth more: the capture per contract
rises by a factor of forty, and the fill count halves.

**A late cancel leaves a stale quote standing.** The quote sits at a price the strategy has already decided
to leave, so the trades that reach it are exactly the trades that were going through that price, which means
buying above the mid. The fill count rises rather than falls, because a quote that is never pulled is always
there to be hit, and the capture per contract turns negative: each fill is worth less than nothing before
fees are counted.

The asymmetry is the lesson. Being slow to arrive costs fills; being slow to leave costs money. One latency
knob would have averaged the two into a mild cost, and the numbers above say the mild version is wrong in
both directions. It is also the reason the two are separate flags rather than one: a strategy that is only
slow on the way in and fast on the way out is a different business from the reverse, and this project can
now tell them apart. `docs/PERFORMANCE_HISTORY.md` records the five-point sweep of each, including the
markout column that shows the cancel's cost is realised at the fill rather than after it.

## Field reference

The fields a reader needs, what they mean, and which line carries them. `[EXECUTION]` is authoritative
for execution quality; `[STRATEGY_METRICS]` carries only what a strategy owns.

| Field | Meaning |
|---|---|
| `submitted_orders`, `submitted_quantity` | Quotes sent to the engine, and their total size |
| `cancel_requests` | Cancellations requested, one per resting order pulled |
| `trade_reports`, `trade_report_quantity` | Fills, and the quantity filled |
| `fill_rate`, `cancel_rate` | Filled quantity over submitted, and cancels over orders |
| `queue_ahead_consumed` | Queue in front of us consumed by trades, measured exactly |
| `buy/sell_queue_from_quantity_changes` | Queue in front of us removed by level-decrease inference |
| `quote_at_touch_orders`, `quote_behind_touch_orders` | Where the quote sat relative to the venue touch |
| `captured_edge`, `adverse_selection` | Raw sums of price differences; the tick and USD fields below are the readable form |
| `captured_edge_per_unit_ticks`, `markout_per_unit_ticks` | Quantity-weighted averages in ticks, comparable with a half-spread |
| `captured_edge_usd`, `markout_usd`, `fees_paid` | The same quantities in the unit fees are charged in |
| `queue_profile_orders`, `queue_zero_orders`, `queue_min_fraction_p50`, `queue_min_fraction_p10`, `queue_end_fraction_p50` | The queue distribution over orders rather than over fills: how many orders were profiled, how many saw their queue reach zero, and the median and tenth-percentile closest approach and end-of-life queue, each as a fraction of the queue the order joined behind. Both fractions are zero on a path whose model gives a quote no queue at rest, which is the L1 replay. |
| `markout_count` | Fills old enough for the five-cycle markout to have resolved |
| `avg_abs_inventory`, `max_abs_inventory`, `inventory_sign_changes` | Inventory path statistics, updated where the position changes |
| `max_abs_exposure_usd`, `starting_cash_usd`, `exposure_over_collateral_fills` | Marked exposure against the collateral the run began with |
| `position_limit`, `risk_rejected_orders` | The pre-trade gate, and how many orders it refused |
| `strategy_net_position`, `strategy_position_mismatches` | The strategy's own position, and any divergence from the pipeline's |
| `working_orders` | Orders still resting when the input ended |
| `[PORTFOLIO]` `cash`, `realized_pnl`, `unrealized_pnl`, `equity`, `fees_paid` | The account, marked to market at the last price |

Every field above is written somewhere under `src/`. `tools/check_written_fields.py` fails the build if
one stops being written, because a field that is read but never written looks exactly like a field whose
value is zero.
