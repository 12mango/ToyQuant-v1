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
then: submitted_orders=182 trade_reports=21 fill_rate=0.115385 cancel_rate=0.901099 captured_edge_per_unit_ticks=0.0952381 captured_edge_usd=0.00156518 fees_paid=0.042 realized_pnl=-0.109283 queue_ahead_consumed=47649 longest_wait_filled_order_id=45 queue_profile_orders=182 queue_zero_orders=21 queue_min_fraction_p50=0.961474 queue_min_fraction_p10=0 queue_end_fraction_p50=0.961474
-->

**1. A fill is a statement about a queue, not about a price.** 182 quotes produced 21 fills, a fill rate of
11.5%, and 90.1% of the quotes were cancelled rather than filled. The distribution over all 182 orders says
why: **the median quote never came closer than 96.1% of the queue it joined behind**, and the tenth
percentile is zero, which is the 21 fills and nothing else. No quote reached the front of its level and went
unfilled — `queue_zero_orders` equals the fill count exactly — so on this window being first in line was
sufficient to trade. The strategy changed its mind long before the queue cleared, which no price-based fill
rule can represent. The trace shows one instance of that; these five numbers count all of them.

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

**The L1 path is a regression fixture, not a second model.** The Binance Trade+BBO replay and the legacy CSV
and UDP inputs are kept so that earlier results stay reproducible and so that the pinned L1 invariants have
something to check. None of them models a queue in front of a quote, so a fill there is a price event. Every
number this document quotes comes from the L2 path, and an L1 fill count answers a different question rather
than the same question twice.

**Absolute timings do not travel between sessions.** The same unchanged binary measured one parser stage at
581 ns in one session and 834 ns twenty minutes later. Only the paired deltas in the ledger are quoted as
evidence, and every absolute number in the performance documents is labelled as one session's state.

## 6. What the next version does

The order below follows one rule: a change is worth doing when it produces a number the gate can check. The
simulator, its fee model, its queue model and its gate are good enough that more *mechanism* would add surface
rather than knowledge, and the weakest part of the project is no longer a component. It is that the strongest
claim here is supported by one order's trace, and that the strategy and risk layers cannot be measured at 0 to
2 fills per window. Every item below adds evidence.

**1. Turn the trace into a distribution.** The trace answers "why did this order not fill" for one order. The
claim it supports is about all of them, so the next step is an aggregation over orders: how many quotes were
pulled with the queue still mostly intact, the distribution of wait times, and the share of fills where the
queue actually reached zero. The machinery is already there — the pipeline knows each order's level and the
engine exposes the queue in front of it — so this is the cheapest item per line of code in the list, and it
converts the project's central claim from an anecdote into a statistic that counts every order rather than
every fill.

**2. Test the conclusions on a day they were not derived from.** A snapshot set for 2020-05-01 sits next to
the 2020-04-01 incremental file, and the L1 path has three days of BTCUSDT from 2024, so an out-of-sample
check does not need new data. The things worth re-testing are the fee-versus-spread arithmetic and the sign of
the captured edge, because those are the conclusions a reader would otherwise be entitled to call one day's
artifact.

**3. Make the economics a frontier instead of a point.** Sweep the quote width against the fee tier and report
captured against paid per contract with the fill count beside it: at what spread does this become viable, and
how much of the answer is the fee assumption rather than the strategy? The unit is per contract, so the result
does not need thirty fills to be stable. It needs one small flag to override the maker rate, in the same shape
as the existing spread overrides, and a table that says where the boundary is.

**4. Pool windows before adding strategies.** No new strategy should be written until the numbers can tell one
apart from another: the regression matrix reports 0 to 2 fills per strategy. A pooled per-contract table over
several windows is the prerequisite, and it is also the point at which the inventory, toxicity and volatility
knobs inside the mainline strategy become design decisions instead of defaults.

**5. Close the parsing question with one bounded experiment.** Parsing is 61% of the event cost, and the ledger
already shows the field split within 12% of a hand-tuned scan and two attempts to tune the row loop paying
nothing, so the one shape change left is parsing a block at a time. Doing it once turns "we do not know" into
"the headroom is not there", which is a result, and it lets the performance line be closed rather than left
open indefinitely. The same applies to the matcher's book container, which is waiting on a workload with order
flow rather than on a container.

**6. Only after item 3: a pre-trade depth filter, not a cancel rule.** The queue distribution invites one
conclusion the measurements already rule out and one they support. Ruled out: do not keep a quote because its
queue is nearly clear. That delays a cancel, and a delayed cancel has been measured — the cancel latency sweep
flips the captured edge from +0.095 to -0.315 ticks at its smallest step, 1 ms, because a quote that outlives
its price is filled at a price the strategy would no longer post. Keeping a near-front quote through a small
price move is the same experiment with the same sign. Supported: the same distribution says that 90.1% of the
cancels land on quotes that never came close, so the decision it should change is **whether to quote at all**.
A level whose queue cannot clear within the strategy's own holding time is a quote that cannot win, and that is
a pre-trade test rather than a cancel rule. It sits behind item 3 because filtering which levels to quote on a
book that loses money per fill only changes how many losing fills there are.

Explicitly not on this list: more strategies, more knobs, or any connectivity, persistence or risk-gateway
work. The non-goals are load-bearing, and each of them would make the loop harder to read without making a
single number more trustworthy.

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
| "What next?" | Section 6: aggregate the one-order trace into a distribution over orders, because the central claim is about all of them; then re-test the conclusions on a day they were not derived from, which this repository has data for. |

### Showing it live

The demo is three commands. The gate first, so the listener knows the numbers are checked: `bash
tools/verify_l2.sh fast`. Then the trace, which the hints turn into a two-step — run once to see which order
waited longest, then name it in a second run, or let `tools/trace_order.sh` do both. Then the two latency
sweeps as a pair, since the surprise is that they disagree in sign.

## Appendix — Technical decisions, module by module

[Architecture](ARCHITECTURE.md) says what each module *is*. This appendix is the argument for the shapes
inside them: the container, the algorithm, or the API form, with the alternative that was measured and lost.
One-off or trivial choices are left out on purpose, and where a decision was deliberately **not** taken it is
listed with its reason, so that "considered and skipped" stays distinguishable from "nobody looked".

Two rules apply to every row. A change here is accepted only with a **paired measurement** and a
**byte-identical output** on the pinned run, because a container swap that is 20% faster and 0.01% different
is not a container swap. And the deciding number is quoted from the ledger in
[Performance](PERFORMANCE.md), not from an isolation benchmark, because those two disagree by up to four
times on this codebase — an example of which is below.

| Module | Choice | The alternative that lost | Deciding measurement | Status |
|---|---|---|---|---|
| line layer (`src/market/line_reader.*`) | 1 MiB blocks, `memchr` boundaries, `string_view` into the block | `std::getline` / `gzgets` per character into a `std::string` | line layer 0.115 -> 0.080 s (1.44x), within 10% of the `memchr`-only floor; end-to-end -16.0% on the 2M slice | kept |
| market book container (`src/orderbook/tick_ladder.h`) | flat array indexed by price tick, learned price range, remembered best slot | `std::map`; **and** a sorted `std::vector` | `book-apply` p50 193 -> 51; in-process book -25.9%; end-to-end -9.5% (t = -2.83) | kept |
| the top-level walk (`TickLadder::top_levels`) | one bounded walk per side | `nth()` per level, each restarting from the best slot | `market-view` p50 95.7 -> 68.4 ns (-28.5%); end-to-end -5.9% (t = -2.61) | kept |
| number parsing (`src/market/market_data_adapter.cpp`) | integer mantissa plus a power-of-ten scale, for the shape this feed uses | `std::from_chars<double>`, which libstdc++ defines out of line | `read+parse` p50 -65.5 ns (-11.3%) | kept |
| engine per-symbol state (`src/exchange/matching_engine.h`) | integer symbol ids, one flat map | string keys on the hot path | `engine` p50 141 -> 111 (-21.3%), 5/5 pairs | kept |
| reader micro-optimizations | four inline update slots, a column-role jump table, first-character boolean dispatch | the straightforward versions | **no measurable effect**: paired mean +5.4 ns, SE 12.4 | kept, unproven |
| matching book per level (`MEOrderBook`) | `std::map<PriceTick, PriceLevel>` | the same flat ladder the market book uses | cannot be measured yet: 406 orders over 117,288 batches | left alone |
| the market book's mutex | `std::mutex` around every call | removing it | 4.2 ns, 0.7% | left alone |

### The line layer, and a piece of reasoning that was wrong

`std::getline` and `gzgets` are both per-character loops, and the feed is 1.5 GB of short rows, so the layer
was replaced before any parsing work: pull a 1 MiB block, find line boundaries with `memchr`, and hand back a
`string_view` into the block. A line crossing a block boundary is compacted to the front, so an owned copy is
only needed for a line larger than a block. All five readers use it, which also deleted the comma-counting
pre-pass the old `split_csv(std::string)` needed to size its reserve.

The appendix mentions it for the mistake as much as for the win. The first write-up argued that the remaining
cost in the field split was `memchr` **call setup** rather than bytes scanned, and the document said so. A
byte loop then measured the same, so the reasoning was wrong: on this data the scan was never the cost. The
correction is still in [Performance History](PERFORMANCE_HISTORY.md) next to the original claim, which is the
only reason it is findable, and it is the reason the ledger keeps a table of attempts rather than a list of
wins.

### The market book container, and the alternative that looked obviously better

The L2 book stores one side as a flat array indexed by price tick. The header states the argument and it is
worth repeating, because it is a *shape* argument rather than a constant-factor one: a red-black tree descends
about ten pointer levels per lookup, and a sorted vector moves kilobytes on every insertion once the book
holds a thousand levels. This feed keeps more than a thousand live levels per side across a span of about
twenty thousand ticks, so both structures pay for a shape the data does not have. Indexing by tick turns a
level change into one computed address and a bounds check.

The interesting part is the alternative that lost. A sorted `std::vector` is the usual first suggestion, and
it was implemented and measured: 1.02 to 1.11x over the `std::map` in isolation, which is not enough to buy a
thousand-element `memmove` per insertion and a binary search per lookup in exchange for a container with
worse failure modes. That measurement is what makes the rejection a result rather than a preference, and it
says something about the tree as well: the two structures pay for the same shape.

Three details inside the class are decisions in their own right:

* **The price range is learned, not configured.** The array is sized around the first tick it sees and grows
  when an update arrives outside the current range, so no window is assumed and no update is dropped. The cost
  is a growth margin that decides how many empty slots a walk crosses; that margin is untuned and is listed as
  such rather than described as chosen.
* **A repeated value writes nothing.** `set()` returns early when the slot already holds the requested
  quantity, which an incremental feed does constantly, and `clear()` skips the fill when the count is already
  zero.
* **The top-level walk is one walk.** `top_levels(levels)` produces the touch and the summed depth of the
  first `levels` populated slots in a single pass, where asking `nth()` for each level restarts from the best
  slot and crossed the near-touch slots about five times over. It is the cheapest accepted change in the
  ledger at -28.5% of its stage and -5.9% end to end, and its output was pinned byte for byte over 11,403,032
  batches.

### Number parsing: faster without being different

Prices in this feed are plain decimals and the book works in integer ticks, so the conversion is done as
`mantissa / 10^decimals` with integer arithmetic. The fast path accepts only shapes it can reproduce
**exactly** — an optional sign, at least one integer digit, at most five fraction digits, and a mantissa a
double holds exactly — and falls back to `std::from_chars<double>` for everything else. The comment in
`src/market/market_data_adapter.cpp` states the guarantee that makes this legitimate, and it is why the pinned
output does not move: both operands are exact and IEEE division is correctly rounded, so the result is the
correctly rounded value of the exact decimal, which is what the general parser would have produced. A trailing
dot, an exponent or a stray character takes the slow path deliberately.

The cost it avoids is not arithmetic but a call. `std::from_chars<double>` is defined out of line in
libstdc++, so it is a real function call per row, about 48 ns in context, and removing it is 11.3% of a stage
that is 61% of the event. Two related attempts in the same file are worth knowing about: a fast path for
64-bit integers paid -9.2%, and an ablation that collapsed the price to two distinct values appeared to pay
259 ns. That second number was wrong by about four times, because collapsing the price also shrank the whole
run's memory footprint, so most of what it measured was cache effect. An ablation measures the call *and*
everything that changes with it, which is the most reusable warning in this repository.

### Per-symbol state, and why the isolation benchmark disagrees with the run

The matching engine used to key its per-symbol state by the symbol string. Keying it by a small integer id
instead, assigned on first sight, moved the `engine` stage from a p50 of 141 to 111 ns (-21.3%, 5/5 pairs,
t = -2.85). The interesting number is the one next to it: the isolation benchmark predicted about 55 ns for
roughly ten string lookups on this path, and the real loop saved about 30. The isolation benchmark measures a
dependency chain in which the hash is the only work; the pipeline's loop overlaps the hash with other work
and pays less for it. Both readings are correct, and only one of them is the number a user experiences, which
is why the ledger quotes in-context differences.

The same map is now the one place where a container choice was deliberately *not* made. `MEOrderBook` still
holds a `std::map` per price level, and the market-data result is the argument for measuring the same flat
ladder there. The obstacle is not the container: this workload submits 406 orders over 117,288 batches, so any
difference would be buried in noise. The prerequisite is a workload with order flow, and it is listed that way
in section 6 rather than left as an open question.

### Small choices that carry a reason

Grouped rather than each getting a section, because each one is a line of reasoning rather than a measurement:

* **`std::stable_sort` for in-flight orders** (`MatchingEngine::advance_to`). Delivery times can collide, and
  when they do the order that was sent first has to land first, or a replay is not reproducible. A plain
  `sort` would leave that to the implementation, so stability is a correctness requirement here rather than a
  preference.
* **`std::deque<FillObservation>` for pending markouts** (`Pipeline`). Markouts are resolved in arrival order
  and drained from the front, so the container has to remove from the front in constant time.
* **`std::variant` plus `std::visit` for market events**, rather than a base class. The set of event types is
  closed and known, so the dispatch is a jump table, the compiler enforces that every alternative is handled,
  and there is no vtable on the event path.
* **Designated initializers for every configuration struct** (`L2MarketMakerConfig`, `FeeSchedule`,
  `StrategyFactoryConfig`). The last of those replaced eleven positional arguments, seven of them doubles, in
  which a transposed pair of the four L1 fractions would still have compiled. Named fields make that class of
  mistake unwritable, and the C++20 rule that initializers follow declaration order is what makes a call site
  readable at review time.

### What was deliberately left alone

* **Reader micro-optimizations** — four inline update slots per batch, a column-role jump table and a
  first-character boolean dispatch. Measured as no effect (paired mean +5.4 ns, SE 12.4). They are kept and
  labelled unproven rather than reverted, because the label is the useful part: the next reader knows the idea
  has been tried.
* **The market book's mutex**, 4.2 ns and 0.7% of the run. Removing it would be a correctness change for a
  measurement that does not exist.
* **The TickLadder growth margin.** It sets how many empty slots a walk crosses and it has never been measured,
  which is stated in [Performance](PERFORMANCE.md) as an untuned knob rather than implied to be tuned.
* **Copying `BboQuote` into three places per batch.** It is a known remaining cost inside the `engine` stage
  and it is recorded as remaining work, not as a decision.

### How a change in this layer is accepted

The same gate as everywhere else, which is what makes the appendix safe to write: a paired measurement
(`tools/measure_l2_baseline.py`, repeated runs, t-statistics) for the speed claim, and byte-identical output
on the pinned run for the behaviour claim. The accepted changes above were each verified as identical over
11,403,032 batches, which is what a container swap has to prove before it is worth its speed. The default
run's full output is hashed as tier 2, so a change here that is faster but different is caught even if nobody
thought to check.









