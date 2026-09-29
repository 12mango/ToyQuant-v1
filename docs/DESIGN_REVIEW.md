# Design Review

This document explains why this simulator is built the way it is. It is written for someone who has to
evaluate the design rather than run the code: the constraints the project holds itself to, the decisions
behind each component, the alternatives that were measured and rejected, what the evidence now says, and
where the design is still weak. [Architecture](ARCHITECTURE.md) describes what exists and
[Performance History](PERFORMANCE_HISTORY.md) records how each number was obtained; this document is the
argument that connects them, and [Learning Path](LEARNING.md) is the same material from the market-making
side rather than the engineering side.

Everything below is a claim about what a small, readable simulator can establish. The project's answer is
that this is more than expected, provided that every number is gated: the headline figures in section 4 are
re-computed by `tools/check_docs.py` on every accepted change, and the document you are reading is checked
for stale flag names and tool paths by `tools/lint_prose.py` exactly like the code is.

## 1. The problem the demo models

A market maker posts two quotes and earns the spread when one is filled. The two easiest things to model
are wrong in the same direction, which is what the design is a reaction to.

**Filling a quote because the price traded there.** A real quote waits behind a queue. A backtest that
fills at the touch is not optimistic by a few percent, it measures a different business: it never waits, so
it cannot see adverse selection, and its fill count becomes a function of how wide the quote was rather
than of how long it survived. The sign of that error is not obvious in advance, either. The project's own
queue model made fills *more* favourable when a quote arrived late, because a late arrival ends up further
from the mid ([Learning Path 7](LEARNING.md)), which is the opposite of the usual assumption that latency
costs money everywhere.

**Reporting profit and loss.** PnL over a few dozen fills is dominated by which way the market moved. It
cannot separate a well-placed quote from a lucky one, and it is the number a reader remembers. The same
window that produces a fill rate of 11.5% produces a realized PnL of -0.109 on 1,000 of starting cash, and
the second number tells you nothing that the first does not.

So the simulator inverts both. Fills come from an explicit queue position, and the headline unit is the
**edge per contract in ticks** next to the **fee per contract** and the **fill count**, so that a result
resting on five fills is visibly a result resting on five fills. The rest of the component list in
[Architecture](ARCHITECTURE.md) exists to make those two inversions possible without giving up the ability
to read the whole loop in one sitting.

## 2. The constraints the project holds itself to

Four rules shaped nearly every later decision, and each one forbids a shortcut that would otherwise have
been taken.

**C1 — The default path stays byte-identical.** Every behaviour knob has an inert default, and
`tools/verify_l2.sh full` hashes the whole standard-output stream of a fixed run against a stored
reference. A change that moves the default run fails the gate even when its own feature works. What it
bought: the last three feature iterations — entry latency, cancel latency, and the order queue trace — all
landed with that reference hash unchanged, because each one is inert at its default.

**C2 — No claim without a gate.** Prose is not trusted. `tools/check_docs.py` re-runs the command a marked
block names and fails if any quoted value has moved; `tools/lint_prose.py` fails when a document mentions
a flag the binary does not accept, omits a flag it does accept, names a queue model that does not exist, or
points at a `tools/` script that is not in the repository. What it bought: the documentation caught two of
the author's mistakes before a reader did, an undocumented flag twice and a paragraph that still described
a model set after the set had changed.

**C3 — Measurements are paired, repeated, and the table includes what failed.** The optimization ledger in
[Performance](PERFORMANCE.md) carries one row per attempt, including rows whose verified effect is "no
measurable effect" and a row for a container that was rejected outright. What it bought: a finding that is
only visible in a table of attempts rather than a list of wins, namely that cost intuition on this codebase
is wrong by a factor of three to ten in a consistent direction, and that several plausible optimizations
paid nothing at all.

**C4 — A red test cannot pass the gate.** This one was learned by failing it. A commit landed carrying a
failing assertion because `tools/verify_l2.sh` compared replay output only: the gate reported success while
the unit tests were red. CTest now runs first, as tier 0, and the first change made under the new gate was
caught by it. What it bought: the gate is the thing that says no, instead of the author's attention.

## 3. The decisions behind the components

Each decision is written as the situation, the alternative that was considered and dropped, the choice,
the evidence, and what the choice costs. The costs are included because a design document that lists only
advantages is a brochure.

### D1 — Simulate the matching instead of inferring fills from prints

**Situation.** A replay-only backtester has no way to represent a quote waiting behind other orders. The
usual shortcut is to fill an order when a trade prints through its price.

**Alternatives.** Filling at the touch; filling through the touch; a fixed fill-probability haircut applied
to the touch. All three are cheap and all three make the fill count a function of prices only, which is
precisely the quantity the project wanted to study.

**Choice.** A matching engine with a book per symbol, price-time priority, partial fills, self-trade
prevention, and a per-level queue a new order joins behind. The engine owns order state and emits
execution reports (`Resting`, `Trade`, `Filled`, `Cancelled`); the portfolio and the strategy's position
both update from those reports rather than from market data.

**Evidence.** The fill count stops being a price function and becomes a queue function. On the 15-minute
regression window the baseline submits 657 orders and fills 2 of them: a quote that is one tick from the
touch and behind a five-figure queue is simply not reachable, and no price-based fill rule can express
that. The pinned invariants for the 200,000-row conservative run are 406 orders, 1 fill and 55,460
contracts of consumed queue position, and they are compared on every code change.

**Cost.** This is the most expensive component in the repository, and its behaviour is a *model* rather
than a reconstruction of one venue's queue. The project's response is to make the model's parameters
explicit and to state its results in units that survive the model being wrong: prices and per-contract
economics, not fill counts or PnL.

### D2 — Make the queue model an explicit knob with bounds, not a constant

**Situation.** The queue rule is where a simulator's honesty lives, and it is invisible if it is compiled
in. Any single rule invites the objection that the results are an artifact of it.

**Alternatives.** One calibrated rule and no discussion; or a rule with a free parameter tuned until the
fill count matched a target number.

**Choice.** Four models behind `--queue-model`: `conservative`, which removes whole level decreases from
the back of the queue and is a lower bound on how fast a queue clears; `prorata`, the calibrated default,
which shares a reduction in proportion to the queue position; `lumpy`, which reproduces the same mean as
pro-rata while giving each level decrease the variance real ones have, controlled by `--queue-chunks`; and
`optimistic`, an upper bound. In addition, `--arrival-share` sets the fraction of the displayed quantity a
new quote starts behind, with 1.0 meaning it joins behind everything showing.

**Evidence.** Two results from measuring the knob rather than arguing about it. First, the arrival share
barely moves the outcome, because the pro-rata share is a fixed point of the update rule: what determines
the outcome is the dynamics at the level, not the opening position. Second, only trades move a level's
queue materially, since quantity changes that are not trades account for about 0.04% of what happens at a
level, so the queue is decided by flow and not by the resting book's churn. Both results are uncomfortable
for anyone expecting a tuned parameter to carry the result, which is the reason the knob was built and
measured instead.

**Cost.** Four models is four code paths to maintain and four sets of numbers, and the default one is still
a calibration rather than a fact about a real venue. The bounds are the mitigation: a conclusion that holds
under both the conservative and the optimistic model does not depend on the calibration.

### D3 — Charge fees on the instrument's face value

**Situation.** The first fee model multiplied the trade price by the quantity, which is right for spot and
wrong by a factor of about 630 for a Deribit perpetual, whose contract carries a fixed 10 USD face value.

**Alternatives.** Leaving it, on the grounds that a toy project does not need exact fees; or excluding fees
from the comparison entirely.

**Choice.** Fees are computed from the instrument's unit notional, so an inverse contract pays a fee on
10 USD rather than on the BTC price, and the fee schedule travels with the engine rather than with the
portfolio.

**Evidence.** The correction is visible as a before-and-after in the history: a strategy comparison that
reported net PnL between -13 and -21 per window was re-measured as -0.02 once the fee model was fixed, so
the earlier table had been dominated by the overcharge and said nothing about the strategies. A unit test
now asserts the fee on a single 10 USD Deribit contract to twelve decimal places.

**Cost.** Every PnL number recorded before the fix is invalid, and the project keeps them in the history
with that label rather than deleting them. That is the point of keeping a history: the invalid numbers are
the evidence that the bug was found by measuring rather than by reading.

### D4 — Model latency as two independent legs

**Situation.** The obvious latency knob is one number applied to "an order". But an order touches the
exchange twice: once when it is sent and once when it is cancelled, and those two events have opposite
consequences for a market maker. A simulator with one knob averages them.

**Alternatives.** A single latency flag applied to "an order"; no latency at all, on the grounds that a
replay drives the clock anyway; or latency applied only to arrivals, which is what most backtests do.

**Choice.** Two flags, `--order-latency-us` and `--cancel-latency-us`, both defaulting to 0, both holding
the action on the exchange's own clock so the same feed rate produces the same latency, and both inert
enough that the default run is bit-for-bit unchanged.

**Evidence.** The two legs move the headline metric in opposite directions. Adding 20 ms of arrival latency
cuts the fill count from 21 to 11 and raises the captured edge per contract from 0.095 to 4.09 ticks, because
a quote that lands late is a quote placed at a price the strategy has since abandoned, which on this feed
means further from the mid and therefore a better price for the side that gets filled. Adding 20 ms of
cancel latency raises the fill count to 25 and turns the captured edge negative, at -0.440 ticks, because
the quote stands through the move the strategy wanted to escape, so the trades that reach it are the ones
going through its price. One number would have averaged a mild improvement with a real cost.

**Cost.** Two knobs instead of one, two sweeps to maintain, and the arrival result contradicts the common
assumption that latency is uniformly bad, which has to be explained rather than quoted. The response is to
report both sweeps in the history and to keep the flags separate so a reader can reproduce either direction.

### D5 — Report execution quality in ticks per contract, with the fee printed beside it

**Situation.** PnL is the number everyone asks for and the number least able to support a conclusion here:
on this workload 21 fills produce a realized PnL of -0.109, which is a statement about the market's drift
over one window.

**Alternatives.** Reporting PnL only; reporting a fill-weighted average price without a reference; or
reporting the spread quoted rather than the spread captured.

**Choice.** The summary carries the quantity-weighted edge per contract in ticks and in the instrument's
fee currency, the same pair for the five-cycle markout, the total fees, and the fill count, all on the same
line. PnL stays in the output because a reader will compute it anyway, and no conclusion in the documents
rests on it.

**Evidence.** With the two figures on the same line, the economics of this policy become arithmetic rather
than opinion, and all of the inputs are gated. The captured edge is 0.0952 ticks per contract. The fees are
0.042 USD over 21 contracts, so 0.002 USD per contract, which is the documented 2 basis point maker rate on
the contract's 10 USD face value. One tick of a 10 USD inverse contract near 6429 is about 10 x 0.5 / 6429,
or 0.00078 USD, which is confirmed by the same run's own units: 0.0952 ticks x 0.00078 USD is 7.4e-5 USD per
contract against a reported 0.00156518 USD over 21 contracts, 7.45e-5. On that conversion the fee is **2.57
ticks per contract against a captured 0.095**: the policy earns about 3.7% of the fee it pays, and the fee
is more than five times the half-spread the quote was placed at. The project could not have said that from
PnL, and the arithmetic above uses only numbers a reader can re-measure.

**Cost.** The summary is longer than a PnL line, and the reader has to be told what a tick-denominated
markout is. It also makes the project's central result a negative one, which is a harder thing to present
than a positive one.

### D6 — Make one order's mechanism visible instead of only its total

**Situation.** Every number the project prints is a summary, and a summary of a queue is the least
informative thing to print, because the interesting behaviour is a sequence: how long a quote waited, what
took the queue in front of it, and why it left the book.

**Alternatives.** A histogram of order lifetimes; a per-order log file; or documenting the mechanism in
prose and trusting the reader.

**Choice.** `--trace-order=N` prints every event that changed the queue standing in front of one order, plus
the reports that ended it, as `[QUEUE_TRACE]` rows. Because a reader cannot know an id in advance, a traced
run also prints `[TRACE_HINTS]` naming the first order that traded and the orders that waited longest. Both
are absent from the default run, so the default output stays byte-identical.

**Evidence.** The trace turned an assertion into an observation. Traced order 3 joined behind 7,200
contracts, spent 2,217 ms waiting, and was cancelled with 169 contracts still standing in front of it, so it
never had a queue position from which it could trade; 86% of everything that left the queue went in two
events, so the queue collapses rather than erodes. The project had claimed all three of those things in
prose beforehand, and the trace is what makes them checkable. It also corrected a belief held while writing
it: the queue counter belongs to a *resting order's* wait and not to a price, so an untouched level reports
zero, which is a semantics detail that only became visible when a single order's life was printed.

**Cost.** It is the only place in the project where a mechanism is displayed as a sequence, so it does not
generalise: it answers "why did *this* order not fill" and not "what is the distribution of waits". And it
is a reporting feature, which means it has to be provably incapable of changing a result, which is why it
prints nothing at all unless it is asked for.

## 4. What the evidence says

The window is the first 200,000 rows of the Deribit incremental book for 2020-04-01 with the matching
trades file, replayed through the mainline `active_l2` strategy under the calibrated `prorata` queue model,
quoting one tick wide and re-quoting after a four-tick move. The block below is the run the numbers in this
section come from. It is executed by `tools/check_docs.py` as part of the ordinary gate, so a claim here
that stops being true fails a build rather than misinforming a reader.

<!-- toyquant:check
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --trace-order=1
then: submitted_orders=182 trade_reports=21 fill_rate=0.115385 cancel_rate=0.901099 captured_edge_per_unit_ticks=0.0952381 captured_edge_usd=0.00156518 fees_paid=0.042 realized_pnl=-0.109283 queue_ahead_consumed=47649 longest_wait_filled_order_id=45
-->

**1. A fill is a statement about a queue, not about a price.** 182 quotes produced 21 fills, a fill rate of
11.5%, and 90.1% of the quotes were cancelled rather than filled. The trace explains why: the order it
follows joined a 7,200 contract queue and was pulled with 169 contracts still in front of it. The strategy
changed its mind faster than the queue cleared, which no price-based fill rule can represent.

**2. Adverse selection takes most of the quoted spread.** The quote is placed half a tick from the mid, and
the captured edge is 0.095 ticks per contract, about 19% of what was quoted. The other 81% is the cost of
being filled preferentially when the market is about to move against the position, and it is the reason the
project measures the markout instead of assuming the spread is earned.

**3. The fee is larger than the spread, and by more than a rounding error.** The fee is 2 basis points on a
10 USD face value contract, 0.002 USD per contract; the captured edge is 0.095 ticks, 7.45e-5 USD per
contract; the total fee paid is 0.042 USD against a total captured edge of 0.0016 USD. This policy earns
about 3.7% of the fee it pays, and the fee alone is more than five times the half-spread available to it.
That is the honest headline of the demo: a queue-aware, fee-aware market maker at a one-tick spread on this
feed is not close to profitable, and the simulator can say exactly by how much.

**4. The two latency legs point in opposite directions.** 20 ms of arrival latency cuts fills from 21 to 11
and raises the captured edge to 4.09 ticks; 20 ms of cancel latency raises fills to 25 and turns the captured
edge to -0.440 ticks. A single latency number would have reported a mild cost and hidden both effects.

**5. A queue collapses rather than erodes.** Of everything that left the traced order's queue, 86% went in
two events out of twenty-three. A model that spread the same total reduction evenly would match the average
and miss the mechanism, which is why the queue model has a variance-controlled mode rather than only a
proportional rule.

**6. Cost intuition on this codebase is wrong by 3 to 10x, in a consistent direction.** The optimization
ledger records plausible optimizations that measured as no effect, an isolation benchmark that overstated a
win by about four times because it changed the memory footprint, and a container that was rejected after
being tested. Engineering judgement here is a hypothesis generator, not a measurement, and the ledger is
kept as a table of attempts because a list of wins cannot show this.

## 5. What is wrong with it

The previous section is the case for the design. This one is the case against it, and it is the section an
interviewer should be pointed at, because a designer who cannot list the weaknesses of their own system is
describing someone else's.

**The fill counts are too small to support a PnL claim, and the project says so instead of quietly doing
it.** The mainline policy fills 21 times in this window and the flat baseline twice. The project's own floor
is thirty fills before a PnL statement is treated as a statement, and none of these tables clears it, which
is why the latency and queue results are reported as per-contract economics rather than as profit.

**The queue is a model, not a reconstruction.** The L2 feed carries aggregated quantities, not order
identities, so the queue in front of a quote is inferred from changes in the displayed size. The model can
say how a queue is likely to clear; it cannot say which order was ahead of which. Everything the project
concludes from the queue has to survive that, which is why the models are bounded and the conclusions are
about prices.

**One venue, one instrument, one day, and windows of 200,000 to 2,000,000 rows.** Deribit BTC-PERPETUAL on
2020-04-01 is a liquid market with a particular queue dynamic and a particular fee schedule. A different
venue would change the queue model's calibration and possibly the sign of some results.

**Nothing downstream of the fill is modelled.** There is no market impact, no queue-position feedback from
the strategy's own resting size, no persistence, no recovery, and no risk gateway. Each is a deliberate
non-goal rather than an oversight, and each would change the numbers if it were added.

**Two known gaps are documented and left in place.** The legacy L1 makers are frozen implementations: their
behaviour is pinned by invariants rather than described, so a change to their internals would be caught by
the regression numbers rather than by a reader, which is a deliberate trade and is labelled as such where
they live. The volatility halt threshold in the mainline L2 strategy is a price speed whose units are a tick
range over a fixed window, so it is not comparable across feed rates; it is left at the value its window was
recorded with, because changing a risk control to make it comparable is a behaviour change that deserves its
own measurement rather than a quiet edit.

**Absolute timings do not travel between sessions.** The same unchanged binary measured one parser stage at
581 ns in one session and 834 ns twenty minutes later. Only the paired deltas in the ledger are quoted as
evidence, and every absolute number in the performance documents is labelled as one session's state.

## 6. What the next version does

In rough priority order, with the argument for the order rather than a wish list.

**1. Parse the depth file in blocks rather than a row at a time.** Parsing is 61% of the event cost and the
two number conversions inside it are already gone, so the remaining shape change is the only lever left.
The two attempts to tune the row loop further both measured as no effect, which is what makes this the next
thing to try rather than more of the same.

**2. Measure the engine's book container on a workload with real order flow.** The ledger's container result
is the argument for replacing the per-price-level `std::map` with the same flat ladder the market-data book
uses, but this workload submits 406 orders over 117,288 batches, so the experiment cannot show anything yet.
The prerequisite is a workload, not a change.

**3. Run the strategy comparison on a window where the strategies actually fill.** The regression matrix
currently reports 0 to 2 fills per strategy, so the inventory, toxicity and volatility knobs inside the
mainline strategy are effectively unmeasured. A wider spread, a tighter requote threshold, or a longer and
more volatile window would all produce a comparison with enough fills to mean something, and only then do
the strategy's own parameters become design decisions rather than defaults.

**4. Give the one-order trace a companion.** It answers "why did this order not fill". The natural
completion is a distribution over orders: how many quotes died behind a queue that never cleared, how long
the survivors waited, and what fraction of fills came from a queue that reached zero. The trace machinery is
already there; what is missing is an aggregation instead of a sequence.

**5. Only then revisit the risk controls.** The halt threshold and the position gate both deserve a
measurement under the workload from item 3, since both are the kind of parameter that looks reasonable and
is unverifiable until fills exist.

## 7. The interview versions

Three lengths of the same story, and then the questions this project invites. The numbers quoted are the ones
in section 4, each re-verified by the build.

### 30 seconds

> I built a C++20 market-making simulator that replays real Deribit L2 depth and trades through a matching
> engine with an explicit queue model, because the two things a normal backtest gets wrong are filling you at
> the touch and summarising everything as PnL. Mine reports the edge per contract in ticks next to the fee per
> contract and the fill count. The headline is that a queue-aware, fee-aware maker at a one-tick spread
> captures 0.095 ticks per contract while paying a fee worth 2.57 ticks, so it earns about 4% of the fee it
> pays — and every number in that sentence is machine-checked by the build.

### 2 minutes

> The demo replays Deribit BTC-PERPETUAL depth and trades into a book, a strategy, a matching engine with a
> per-level queue, and a portfolio. Two design choices do the work. First, fills come from queue position
> rather than from a trade printing at my price, because a quote behind a 7,200 contract queue is a different
> instrument from one that fills instantly: it produced 21 fills from 182 quotes on the window I use, and I
> can show you one order's entire wait as a trace. Second, I report per-contract economics rather than PnL,
> because 21 fills cannot support a PnL claim — that policy captures 0.095 ticks per contract while the maker
> fee is 2.57 ticks of the same contract, so it earns about 4% of the fee, and PnL would have hidden that
> behind one window's drift.
>
> The method is what I would actually point at. Every behaviour knob defaults to inert and the full output of
> a fixed run is hashed against a stored reference, so I added order latency, cancel latency and a queue trace
> without moving the baseline. Documented numbers are re-derived by the build, prose is linted against the
> flags and models the code actually has, and the unit tests run first in that gate — I added that after a
> commit of mine shipped a failing assertion while the gate reported success. The measurements are paired, and
> the table includes attempts that paid nothing: cost intuition on this codebase was wrong by 3 to 10x, always
> in the same direction, and that finding only exists because I recorded the failures.

### 10 minutes

Walk it in this order, and stop wherever the questions go:

1. **The loop and the boundary.** Feed adapters, order book, strategy, matching engine, execution reports,
   portfolio. Say what is a non-goal early, so the scope is set by the listener rather than defended later.
2. **The two inversions.** Queue-based fills and per-contract reporting, with the fill count and the fee
   arithmetic from section 4 on screen.
3. **One decision in depth.** The latency pair is the best one: two flags, opposite signs, one number would
   have averaged them into a mild cost, and both sweeps are in the history.
4. **The gate stack.** Four tiers, byte-identical defaults, marked blocks that re-run their commands, prose
   lint, and the red-test hole that was found and closed. This is usually the part that gets taken seriously,
   because it is the part most toy projects do not have.
5. **The mechanism view.** Run the trace live for the order the hints name. It is the only part of the demo
   that is more convincing in a terminal than on a slide.
6. **The honest ending.** Section 5, on the fill-count floor, the queue model being a model, and the two
   labelled gaps. Ending on the weaknesses is stronger than being caught on them.

### The questions this invites

| Question | The answer |
|---|---|
| "Isn't your queue model just made up?" | It is a model, and it is bounded: four rules from a conservative lower bound to an optimistic upper bound, plus an arrival share. Two measurements make it less arbitrary than it sounds: the starting share barely matters, because the pro-rata share is a fixed point of the update rule, and non-trade quantity changes account for about 0.04% of what happens at a level, so flow decides the queue. |
| "Why is your PnL negative?" | Because the fee is 2.57 ticks of a contract whose quoted half-spread is 0.5 ticks. The negative result is the finding rather than a bug: at a one-tick spread on this feed the strategy cannot pay its fees, and that is arithmetic on gated numbers rather than one window's drift. |
| "How do you know these numbers are right?" | The build re-runs the command each number comes from and fails if a value moved; prose is checked for flags, queue models and tool paths that do not exist; the unit tests run before everything else; and the default output is hashed against a reference, so a feature cannot quietly change a baseline. |
| "What was the hardest bug?" | A fee model that charged price times quantity on an inverse contract, about 630x too much, which turned an entire strategy comparison into a measurement of the overcharge. It was found by writing down what a fee should be rather than by reading the code, and the invalid table is kept in the history with that label. |
| "Have you built anything like this for real?" | No, and the document says so: there is no market impact, no connectivity, no risk gateway and no persistence. What is real is the method — a pinned baseline, measured rather than asserted claims, and failures recorded next to successes. |
| "Is this HFT?" | It is a simulator that models the economics of latency, not a low-latency system. The latency work is about which leg of an order's life costs money, and the answer is that the cancel leg does while the arrival leg can help. |
| "What surprised you?" | Three things, all measured rather than assumed: a single latency number hides effects of opposite sign; a queue collapses in a few events instead of eroding, so an average is the wrong summary of it; and my own cost intuition was off by 3 to 10x in a consistent direction, which is why the ledger is a table of attempts. |
| "What would you do differently?" | Build the gate before the features. It caught two undocumented flags, a stale paragraph and a red test — all of them mine — and each was found later than it should have been. I would also have started with the fee model and the queue model, because everything downstream is a measurement of those two. |
| "What next?" | Section 6: parse in blocks, because parsing is 61% of the event cost and the row loop has already paid nothing twice; then run the strategy comparison on a window with enough fills that the strategy's own knobs become measurable. |

### Showing it live

The demo is three commands. The gate first, so the listener knows the numbers are checked: `bash
tools/verify_l2.sh fast`. Then the trace, which the hints turn into a two-step — run once to see which order
waited longest, then name it in a second run, or let `tools/trace_order.sh` do both. Then the two latency
sweeps as a pair, since the surprise is that they disagree in sign.






