# Performance History

This is the chronological lab notebook of the performance work: what was measured, in what order, and
what it said at the time. Most of it has been **superseded**, and that is the point of keeping it
separate.

Where things live now:

| Question | Document |
|---|---|
| What does the engine cost today, and how is that measured? | [Performance](PERFORMANCE.md) |
| Why is a given design faster, and what was rejected? | [Low-Latency Design](LATENCY_DESIGN.md) |
| What was measured earlier, and what did it say then? | this document |

Read a number here only as history. The entries that still hold are repeated in the
[optimization ledger](PERFORMANCE.md#5-optimization-ledger); anything not in that ledger was either
superseded by a later measurement or was never a latency claim in the first place.

The order below is the order the work happened. It starts with a reproducible end-to-end baseline
taken before any latency-focused change, which was a regression reference rather than a production
throughput or latency claim.

## Workload

- Executable: `out/build/linux-debug/strategy_benchmark`
- Trades: `data/v2/test_aggTrades_5k.csv` (3,412 data rows)
- BBO: `data/v2/test_bookTicker_5k.csv` (35,310 data rows)
- Symbol and quantity scale: `BTCUSDT`, `1000000`
- Each invocation replays the inputs sequentially for five L1 strategy variants and writes intermediate order/trade files under `/tmp`.
- Trades SHA-256: `f3dc0bcc056dbe27ddfdae330d9e9fa6ebbacd787a3d71692a898ba762450ca3`
- BBO SHA-256: `0b795bd584c16c3a3049690bf4149e5b8aadddbc1f6dc3f6680a295b75a35b1e`

## Environment

- Linux x86_64 under WSL2
- CPU: Intel Core Ultra 7 258V, 8 online CPUs
- CMake preset/build directory: `linux-debug`
- `CMAKE_BUILD_TYPE=Debug`; project compiler options also include `-O2 -march=native` and Debug adds `-g`.
- `/usr/bin/time` is available. `perf` and `valgrind` are unavailable. `gprof` is installed, but the current binary is not built with `-pg`.

## Measurement

The executable was run seven times with the same inputs. Wall-clock seconds, including process startup, input parsing, all five replays, and temporary output writing:

| Run | Seconds |
|---:|---:|
| 1 | 0.20 |
| 2 | 0.14 |
| 3 | 0.15 |
| 4 | 0.16 |
| 5 | 0.16 |
| 6 | 0.16 |
| 7 | 0.16 |

Median: `0.16 s`; range: `0.14–0.20 s`. Captured stdout/stderr hashes matched across all seven runs, indicating deterministic benchmark output for this workload.

Reproduce from the repository root:

```bash
/usr/bin/time -f '%e' ./out/build/linux-debug/strategy_benchmark \
  data/v2/test_aggTrades_5k.csv \
  data/v2/test_bookTicker_5k.csv BTCUSDT 1000000
```

## The L1 Line, Before the Queue Model

The project began as a Binance Trade+BBO replay with a legacy CSV and a legacy UDP tick path, and the strategy
line that came with it (`naive`, `passive_l1`, `inventory_aware_l1`, `flow_aware_l1`, `active_l1`,
`optimized`) is still in the repository. Everything in this section is history: the design it describes was
superseded, and the numbers stay because the L1 invariants are still one of the sets the fast gate compares.

**What an L1 fill meant.** The feed carries trades and a best bid and offer, with quantities but no depth. The
engine has no level to attach a queue to, so a resting order starts at the touch with nothing ahead of it and
a trade printing through its price fills it. That is a price-touch model, and it answers a real question: did
the market trade at or through my price. It cannot answer the question that decides whether a quote gets
filled, which is what happened to the queue standing in front of it.

**What it measured.** An L1 run is pinned as the second invariant set in the fast gate: 13,734 orders, 2,277
fills, a fill rate of 0.175872, realized PnL of -33.7861 and equity of 966.196 on the recorded input, compared
on every code change. That is the role the line has now. On strategy results the earlier note stands: an L1
sample is unsuitable for profitability conclusions, and nothing here changes that.

**Why the mainline moved to L2.** The question worth answering turned out to be the queue, and a top-of-book
feed cannot see one. The Deribit depth replay added a book with levels, a per-level queue that a new order
joins behind, and the queue models the rest of these documents measure. The same strategy code was then used
to ask whether a fill is a price event or a queue event, and the answer — a median quote that never came
within four percent of the front, on the window the main tables use — is the result this project is built
around.

**What stayed.** The L1 names remain available, the legacy CSV and UDP inputs remain as demo data, and the L1
invariants remain in `tools/verify_l2.sh fast`. New strategy work belongs to the L2 path, and a change to the
legacy makers that moves those numbers is a change to a recorded result rather than an improvement. The
documentation that describes the line as it still runs is in
[User Guide](USER_GUIDE.md#current-modes) and [Architecture](ARCHITECTURE.md).

## Quote Width and the Fee Frontier

The mainline policy quotes one tick wide while the fee is worth 2.57 ticks of the same contract, so the
strategy question with an answer is not "can it beat the market" but "where does the fee stop being the whole
story". The knob is `config_.base_spread`, and widening it moves both sides away from the fair price before
the tick rounding and the clamp that stops a quote from being more aggressive than the touch:

```cpp
// src/strategy/l2_market_maker.h:295
const double raw_bid_price = std::floor((fair_price - config_.base_spread - inventory_shift -
                                         weak_flow_spread_shift) / config_.tick_size) * config_.tick_size;
const double raw_ask_price = std::ceil((fair_price + config_.base_spread - inventory_shift +
                                        weak_flow_spread_shift) / config_.tick_size) * config_.tick_size;
```

Sweep on the 200,000-row window, `--refresh-price-ticks=4`, `prorata`:

| `--base-spread-ticks` | fills | fill rate | captured ticks/contract | captured USD | fees USD | realized USD |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 21 | 11.5% | 0.095 | 0.00157 | 0.042 | -0.109 |
| 2 | 13 | 5.9% | 0.154 | 0.00156 | 0.026 | -0.052 |
| 3 | 5 | 1.8% | 1.2 | 0.00470 | 0.010 | -0.017 |
| **5** | 1 | 0.35% | 5.5 | 0.00432 | 0.002 | -0.002 |
| 8 | 1 | 0.35% | 8.5 | 0.00667 | 0.002 | -0.002 |
| 12 | 1 | 0.35% | 12.5 | 0.00981 | 0.002 | -0.002 |

<!-- toyquant:check
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=5 --refresh-price-ticks=4
then: submitted_orders=285 trade_reports=1 fill_rate=0.00350877 captured_edge_per_unit_ticks=5.5 captured_edge_usd=0.00431508 fees_paid=0.002 realized_pnl=-0.002
-->

Three things fall out of the table, and the third is the one that matters.

1. **Captured edge per contract rises with width**, from 0.095 to 12.5 ticks. A quote further from the mid is
   a better price on the side that gets filled, which is the same mechanism the arrival latency sweep found.
2. **Fees fall** from 0.042 to 0.002 USD, because they are charged per fill and the fills disappear.
3. **The two cross at about five ticks**, and that is the result. At one tick the strategy earns 3.7% of the
   fee it pays; at five ticks it earns 2.2 times the fee for the first time. It cannot have both, because the
   width that pays the fee is the width at which the queue in front stops clearing: the queue distribution's
   median closest approach is 0.961 at one tick and exactly 1.0 from two ticks upward, so the deeper quote is
   a quote that never reaches the front at all.

**The fill counts are the caveat, and they forbid any stronger reading.** At 5, 8 and 12 ticks the run
produces one fill each, and the identical fill rate means the three runs submitted the same 285 orders and
filled one of them. Those rows establish a direction, not a result. The project's floor is thirty fills, so
the honest sentence is: at a width that can pay the fee, this window holds one observation, and the reason is
structural rather than a matter of tuning.

### The same sweep on ten times the window

The 200,000-row window cannot settle the question, so the same sweep runs on the 2,000,000-row slice of the
same file, which is the same data at ten times the length. At one tick it clears the thirty-fill floor:

| `--base-spread-ticks` | fills | fill rate | captured ticks/contract | captured USD | fees USD | realized USD |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 267 | 12.6% | 0.088 | 0.0186 | 0.534 | -1.387 |
| 3 | 91 | 2.9% | 0.181 | 0.0131 | 0.182 | -0.365 |
| 5 | 3 | 0.08% | 1.67 | 0.00395 | 0.006 | -0.013 |
| 8 | 1 | 0.03% | 6.5 | 0.00515 | 0.002 | -0.002 |
| 12 | 1 | 0.03% | 10.5 | 0.00831 | 0.002 | -0.002 |

<!-- toyquant:check
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
then: submitted_orders=2126 trade_reports=267 fill_rate=0.125588 captured_edge_per_unit_ticks=0.088015 captured_edge_usd=0.0186193 fees_paid=0.534 realized_pnl=-1.38723
-->

Two conclusions, and the second is the one a strategy review has to accept.

1. **With enough fills the answer is unambiguous.** At one tick the policy pays 0.534 USD of fees and captures
   0.0186 USD of edge, twenty-nine times less, for a realized loss of 1.387 USD on 1,000 of starting cash. That
   is the fee structure, not a tuning problem: the same run's `queue_min_fraction_p50` is 0.965 and 275 of its
   orders reached the front, so the strategy is quoting where the flow is and still cannot pay for the fills.
2. **Widening the quote does not create value, it reduces participation.** The edge per contract rises by two
   orders of magnitude, from 0.088 to 10.5 ticks, while the total captured value *falls* from 0.0186 to
   0.0083 USD. The realized loss shrinks for the same reason, because within ten ticks the run produces one
   fill instead of 267. A five-fold improvement in realized PnL that comes from not trading is not an
   improvement, and it is the reason the wider rows above are reported with their fill counts beside them.

The 2M table is covered by the second marked block, which adds 1.5 seconds to the fast gate: the slice is a
`head -n` prefix of the tracked file, so the run is one command and no extra data, and the strongest strategy
claim in the repository is worth checking on every change.

## The Levers, and the Invariant None of Them Move

Every knob the strategy exposes was swept on the 2M-row slice with enough fills to read: quote width,
requote threshold, order size and inventory limit. The two remaining sweeps are here, and both were chosen
because they are the ones a reviewer would ask about next.

**The four L2 strategies on the same config** (`--base-spread-ticks=1 --refresh-price-ticks=4 prorata`):

| Strategy | submitted | fills | fill rate | captured ticks/contract | captured USD | fees USD | realized USD |
|---|---:|---:|---:|---:|---:|---:|---:|
| `passive_l2` | 4326 | 432 | 10.0% | -0.102 | -0.0349 | 0.864 | -2.128 |
| `inventory_aware_l2` | 3333 | **635** | **19.1%** | **-0.221** | -0.111 | **2.137** | **-2.578** |
| `flow_aware_l2` | 2052 | 289 | 14.1% | 0.057 | 0.0131 | 0.578 | -1.512 |
| `active_l2` | 2126 | 267 | 12.6% | **0.088** | **0.0186** | **0.534** | **-1.387** |

Two things to read. The mainline choice is now evidence rather than a claim: `active_l2` has the highest
captured edge per contract, the lowest fees and the smallest loss, and the strategy that maximises fills
(`inventory_aware_l2`, 635 of them at 19.1%) has the most negative edge per contract, the highest fees and the
worst result. Fills obtained by being easier to hit are the fills that arrive when the market is moving
against the quote, which is the adverse-selection result this project is built on, reproduced across four
strategies instead of argued from one.

**Order size and inventory limit** (`active_l2`, same config). Both defaults are derived rather than chosen,
which is why this sweep exists:

```cpp
// src/app/strategy_factory.cpp:65
//   instrument->quantity_scale = 1 for Deribit contracts, so:
//   order_size      = max(min_order_quantity, quantity_scale / 1000) = 1 contract
//   inventory_limit = max(1, quantity_scale / 10)                    = 1 contract
```

| `--order-size`/`--inventory-limit` | fills | fill rate | captured ticks/contract | captured USD | fees USD | realized USD | max abs inventory |
|---|---|---:|---:|---:|---:|---:|---:|
| 1 / 1 | 267 | 12.6% | 0.088 | 0.019 | 0.534 | -1.39 | 1 |
| 10 / 10 | 365 | 11.0% | 0.122 | 0.286 | 5.93 | -13.9 | 19 |
| 10 / 50 | 351 | 9.4% | 0.133 | 0.311 | 5.91 | -13.0 | 57 |
| 50 / 50 | 387 | 10.1% | 0.113 | 1.265 | 28.2 | -69.6 | 97 |
| 50 / 200 | 368 | 8.8% | 0.152 | 1.695 | 28.1 | -59.6 | 210 |

**The invariant.** Scale multiplies everything and changes no ratio. Captured value rises about fifteenfold
from the smallest row to the largest, fees rise about elevenfold, and the loss rises tenfold, while the
captured edge per contract moves only from 0.088 to 0.152 ticks. Divided out, the two gated numbers above say
it directly: 0.0186193 USD over 267 contracts is 6.97e-5 USD per contract captured, and 0.534 USD over the same
267 is 0.002 USD per contract paid. The fee is 29 times the edge at one contract, and at fifty contracts it is
still seventeen times, because both sides scale together. **The loss is a rate, not a parameter**, which is
why four sweeps all end in the same place, and why the honest strategy report is a frontier rather than an
improvement.

Both tables are reproduced by the commands in their sections above; their inputs to the invariant are the two
numbers in the marked block, which the fast gate re-computes on every change.

## Out of Sample: the Same Policy on Five Days

Every conclusion above comes from one day, and four more days of the same instrument were added later: a 25-level
snapshot file and a trades file for the first of each month from 2020-05-01 to 2020-08-01. All five days are
replayed through the **same depth format**, which is the point of the design — a difference between the rows below
is the day, not the feed.

| Window | Depth source | submitted | fills | fill rate | captured ticks/contract | captured USD | fees USD | fees / captured | realized USD | queue min-fraction p50 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 04-01 | snapshot (control) | 1146 | 146 | 12.7% | 0.086 | 0.00987 | 0.292 | 29.6x | -0.757 | 0.992 |
| **05-01** | snapshot (day two) | 526 | 58 | 11.0% | 0.034 | 0.00114 | 0.116 | **102x** | -0.287 | 0.951 |
| **06-01** | snapshot (day three) | 306 | 31 | 10.1% | 0.242 | 0.00397 | 0.062 | 15.6x | -0.115 | 0.905 |
| **07-01** | snapshot (day four) | 196 | 29 | 14.8% | 0.379 | 0.00604 | 0.058 | **9.6x** | -0.083 | 0.906 |
| **08-01** | snapshot (day five) | 339 | 26 | 7.7% | 0.462 | 0.00531 | 0.052 | 9.8x | -0.063 | 0.991 |
| 04-01 | incremental (mainline) | 182 | 21 | 11.5% | 0.095 | 0.00157 | 0.042 | 26.8x | -0.109 | 0.961 |

<!-- toyquant:check
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
then: submitted_orders=1146 trade_reports=146 fill_rate=0.1274 captured_edge_per_unit_ticks=0.0856164 captured_edge_usd=0.00987095 fees_paid=0.292 realized_pnl=-0.757038 queue_min_fraction_p50=0.991843
run: l2_replay data/v2/deribit_trades_2020-05-01_BTC-PERPETUAL.csv.gz /tmp/snap_0501_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
then: submitted_orders=526 trade_reports=58 fill_rate=0.110266 captured_edge_per_unit_ticks=0.0344828 captured_edge_usd=0.00113946 fees_paid=0.116 realized_pnl=-0.287468 queue_min_fraction_p50=0.951289
run: l2_replay data/v2/deribit_trades_2020-06-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200601_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
then: submitted_orders=306 trade_reports=31 fill_rate=0.101307 captured_edge_per_unit_ticks=0.241935 captured_edge_usd=0.00396559 fees_paid=0.062 realized_pnl=-0.114748 queue_min_fraction_p50=0.904849
run: l2_replay data/v2/deribit_trades_2020-07-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200701_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
then: submitted_orders=196 trade_reports=29 fill_rate=0.147959 captured_edge_per_unit_ticks=0.37931 captured_edge_usd=0.00603727 fees_paid=0.058 realized_pnl=-0.0832212 queue_min_fraction_p50=0.905757
run: l2_replay data/v2/deribit_trades_2020-08-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200801_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4
then: submitted_orders=339 trade_reports=26 fill_rate=0.0766962 captured_edge_per_unit_ticks=0.461538 captured_edge_usd=0.00531196 fees_paid=0.052 realized_pnl=-0.0633193 queue_min_fraction_p50=0.990675
-->

What survives is the reason for doing it.

1. **The fill rate stays between 8 and 15% on all five days**, so "a quote rarely fills" is not a property of one
   day.
2. **The captured edge per contract stays positive and below one tick** (0.034 to 0.462 ticks), while the fee per
   contract is fixed at 0.002 USD by the fee schedule and cannot move.
3. **The fee is between 9.6 and 102 times the captured edge.** An earlier version of this section said "never
   below 27 times", on three windows; days four and five fell below that at 9.6 and 9.8 times, so the bound is the
   measured range rather than the three-window minimum. The direction of the claim does not move: on every day the
   fee is at least an order of magnitude larger than what the fills capture, and every realized PnL is negative.
4. **The realized loss scales with the fill count, not with the parameters.** 21 fills lose 0.109 USD and 146
   fills lose 0.757 USD, about seven times the loss for about seven times the fills. The added days repeat it: 58
   fills lose 0.287 USD against 26 fills losing 0.063 USD. That is the same scale-invariance the order-size sweep
   found, reproduced on days the parameters were not chosen on.

### Out of Sample: the One Surviving Corner Does Not Travel

The sweep left one corner positive, so it was taken out of the five days it was selected on. On the same instrument
it holds: the same day's other window, two million records instead of two hundred thousand, returns +0.073 USD on 19
fills. On the second venue it does not trade at all, because the gate asks a quote to cover the fee and OKX's tick is
0.1 on a price near 93000, so the fee is about 186 ticks and no reachable quote covers it, which is why the run
submits nothing.

| run | orders | fills | fee USD | PnL USD |
|---|---:|---:|---:|---:|
| five Deribit days, snapshots, 200k | 372 / 62 / 56 / 23 / 37 | 12 / 2 / 0 / 2 / 1 | 0.034 | **+0.033** |
| 2020-04-01, the other window, 2M | 502 | 19 | 0.038 | **+0.073** |
| OKX 2026-01-07, one hour | 0 | 0 | 0 | 0 |
| OKX 2026-01-11, one hour | 0 | 0 | 0 | 0 |

That is the answer to the question the sweep raised. The corner is not a strategy, it is the fee floor written as a
policy: where the fee is a couple of ticks it becomes "quote rarely and well", and where the fee is two orders of
magnitude larger it becomes "never quote", which is the same conclusion the fee model, the coverage sweep, the
conditional rule and the bucketed markout reached separately. One hundred and fifty settings were searched to find
it, which is also why a positive total from ten or twenty fills cannot be read as an edge.

### The Mechanism Space, Swept to Its Edge

Every lever that decides when and where to quote, crossed with the others, on all five days: a base spread of 1, 2,
3, 5 or 8 ticks, a minimum spread of 0 or 2 ticks, and a fee-covering requirement of 0, 0.001 or 1 tick. One hundred
and fifty runs in total.

| fills in a run | runs | fills | PnL USD | PnL per fill |
|---|---:|---:|---:|---:|
| 0 to 2 | 116 | 80 | -0.023 | -0.00029 |
| 3 to 10 | 13 | 71 | -0.676 | -0.00952 |
| 11 to 30 | 13 | 247 | -1.036 | -0.00420 |
| 31 to 100 | 6 | 260 | -1.176 | -0.00452 |
| over 100 | 2 | 248 | -1.194 | -0.00481 |

Every band loses money in total, which is the same statement the earlier sweeps made from three other directions:
participation is what pays the fee, and the fee is larger than what participation earns. The loss per fill is worst
in the band where a setting trades a handful of times and best where it trades constantly, so no corner of this
space is both busy and profitable.

One corner is positive on four of the five days: a one tick base spread with a two tick minimum spread and a
fee-covering requirement of 0.001 tick. It trades 17 times across the five days for +0.033 USD. It is recorded
because it is the only corner that survives the sweep, not because it is a business: that is about three fills a
day, its total is dominated by a single day, and the size is the venue minimum, so it cannot be scaled. Treating it
as an edge would be the mistake this section exists to prevent.

### The Conditional Rule: Refusing a One Tick Book

The bucketed markout says which fills are bad rather than only how bad the population is: fills that landed a tick
or more away from the midpoint resolved positive on all five days, while fills at the midpoint resolved at or below
zero, because a one tick book leaves the touch half a tick from the midpoint and that is where a passive quote gets
picked off. `--min-spread-ticks` acts on that: when the top of book is narrower than the value asked for, nothing is
quoted at all. A zero value leaves it off, so every run recorded above keeps its numbers.

| Day | Condition | submitted | fills | fees USD | realized USD |
|---|---|---:|---:|---:|---:|
| 04-01 | none, and no gate | 1146 | 146 | 0.292 | -0.757 |
| 04-01 | spread at least two ticks | 737 | 35 | 0.070 | -0.097 |
| 04-01 | two ticks plus fee cover | 372 | 12 | 0.024 | **+0.00085** |

Two things are worth saying plainly. The loss falls by four to eight times, and the loss **per fill** falls too, so
the rule is not only trading less. But a wider condition is not a better one: asking for three ticks on 04-01 was
worse than asking for two, and the far bucket's markout turned negative under it, so the condition changes which
fills arrive rather than only how many. That is the reason the rule is a decision about the situation and not a
parameter to be increased.

<!-- toyquant:check
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --min-spread-ticks=2
then: submitted_orders=737 trade_reports=35 fees_paid=0.07 realized_pnl=-0.0967336
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --min-spread-ticks=2 --edge-cover-ticks=0.001
then: submitted_orders=372 trade_reports=12 fees_paid=0.024 realized_pnl=0.000845982
-->

### The Decision Layer: Refusing to Quote

Every row above comes from a policy that quotes whenever it can. The fee is between 9.6 and 102 times what a fill
captures, so the arithmetic says a quote that cannot pay for itself should not be placed at all, and that is a
decision rather than a parameter: `--edge-cover-ticks` requires a quote to capture the maker fee, measured in
ticks at the reference price, plus the ticks the flag names. A zero value leaves the gate off, which is why every
run above keeps its numbers.

| Day | Gate | submitted | fills | captured USD | fees USD | realized USD |
|---|---|---:|---:|---:|---:|---:|
| 04-01 | off | 1146 | 146 | 0.00987 | 0.292 | -0.757 |
| 04-01 | fee-covering | 463 | 10 | 0.00474 | 0.020 | **-0.168** |
| 05-01 | off | 526 | 58 | 0.00114 | 0.116 | -0.287 |
| 05-01 | fee-covering | 121 | 5 | 0.00345 | 0.010 | **+0.002** |

Refusing to quote removes about nine tenths of the fills and with them about nine tenths of the fee, which is the
whole point: it turns a slow loss into roughly nothing, and on the second day into a slightly positive number.
Five fills on one day is not evidence of an edge, and the gate as written covers the fee only, not the adverse
selection a fill suffers, which is the next thing the same gate should carry.

<!-- toyquant:check
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --edge-cover-ticks=0.001
then: submitted_orders=463 trade_reports=10 captured_edge_usd=0.00474021 fees_paid=0.02 realized_pnl=-0.167803
run: l2_replay data/v2/deribit_trades_2020-05-01_BTC-PERPETUAL.csv.gz /tmp/snap_0501_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --edge-cover-ticks=0.001
then: submitted_orders=121 trade_reports=5 captured_edge_usd=0.00345258 fees_paid=0.01 realized_pnl=0.00218216
-->

One claim needs narrowing rather than repeating. On the mainline window `queue_zero_orders` equalled the fill
count exactly, which supported "every order that reached the front traded". On the second day it is 59 orders
against 58 fills, and on the control 150 against 146, so the honest form is **almost every** order that reached
the front traded, within a few percent on all three windows. The direction survives; the exact equality was a
property of one window.

## What the Queue Model Does Not Change

The queue in front of a quote is the weakest part of this simulator, and the obvious upgrade is
order-by-order data. Before spending anything on it, the four queue models the engine already has answer the
question: the truth lies somewhere between `conservative` and `optimistic`, so a conclusion that holds at both
ends cannot be moved by knowing the truth.

Same window, same policy, one row per model:

| Queue model | fills | fill rate | captured ticks/contract | captured USD | fees USD | realized USD | queue min-fraction p50 |
|---|---:|---:|---:|---:|---:|---:|---:|
| `conservative` (lower bound) | 103 | 4.2% | -0.083 | -0.0067 | 0.206 | -0.537 | 1.000 |
| `prorata` (default) | 267 | 12.6% | 0.088 | 0.0186 | 0.534 | -1.387 | 0.965 |
| `lumpy` | 269 | 12.5% | 0.063 | 0.0134 | 0.538 | -1.354 | 0.996 |
| `optimistic` (upper bound) | 305 | 14.0% | 0.111 | 0.0269 | 0.610 | -1.446 | 0.952 |

Three readings, and the third is the one that decides whether finer data is worth buying.

1. **The timing conclusions are model-dependent.** Fill counts move by a factor of three across the bounds
   (103 to 305) and the number of orders that reached the front of their queue moves by a factor of six, so
   those numbers are always reported next to the model that produced them.
2. **The economics barely move, and never in the strategy's favour.** The captured edge stays between -0.083
   and +0.111 ticks while the fee is fixed at 0.002 USD per contract by the fee schedule. In the three rows
   where the edge is positive the fee is 23 to 40 times it, and in the conservative row the edge is negative,
   so there the fee is simply lost. The realized loss grows monotonically with the fill count, which is the
   same scale-invariance the order-size sweep and the second day both showed.
3. **Knowing the true queue cannot change the binding result.** The truth lies inside this range, and the
   range is unprofitable at every point, so finer data would buy precision on the fill count and on the queue
   percentiles rather than a different conclusion. Its one honest use here would be calibration — a day of it
   offline, to fit the four rules — instead of a new runtime path, and only if the goal becomes proving the
   queue reconstruction itself.

## L2 Mainline Workload

To cover the current mainline, `active_l2` was replayed against a 15-minute incremental Deribit
window with the conservative queue model and no depth sampling:

- Window: `[1585699200000000, 1585700100000000]`
- Strategy / queue model: `active_l2` / `conservative`
- Trades: 1,354 rows; sliced input SHA-256 `af5475b5d26329563baab29519b02f440f700f74f13bd822cbcbd682605a6964`
- Incremental depth: 201,130 rows; sliced input SHA-256 `716a84d48138648ccfd904cadfd434b6822070d4675fed88b5ca3545b200ff29`
- Runtime summary: 119,364 events, including 118,010 incremental batches; one batch is a snapshot/reset.

Five runs took `0.39`, `0.35`, `0.37`, `0.35`, and `0.33` seconds (median `0.35 s`, range
`0.33–0.39 s`). Captured output hashes matched across all five runs:
`afddc9b1741da4ac3000ed476ce406b3ad66d7bcf2d0b05a2cd19d9ccdfcccc3`.

The first parser optimization replaced the per-row `std::istringstream` splitter with an equivalent
delimiter scan and added allocation-free fast paths for common boolean fields. On the same
15-minute input, three post-change runs took `0.20`, `0.22`, and `0.17` seconds (median `0.20 s`),
with the same execution summary and identical output hashes. This is an initial comparison against
the five-run pre-change median, not a final speedup claim; repeat the one-hour workload after the
next change for the official comparison.

Incremental batches now reserve two update slots before appending rows. In the measured window,
117,945 of 118,010 batches contained one or two updates, so this avoids common small-vector
growth without reserving eight slots for nearly every batch. The reserve-size comparison was noisy
at this short duration; the choice is based on the observed distribution and memory behavior, not
on a claimed standalone speedup.

The incremental reader also reuses the CSV field vector while collecting rows in one batch. Three
follow-up runs took `0.20`, `0.20`, and `0.18` seconds with identical output hashes. The difference
from the preceding `0.20`, `0.21`, and `0.20` second sample is within short-run noise; the main
benefit is reducing repeated vector allocation on the hot path.

The integer timestamp, sequence, and amount conversions in the adapter now use `std::from_chars`
through a checked `parse_uint64` helper instead of `std::stoull`. The focused adapter and replay
tests passed, and three fixed-window runs took `0.18`, `0.19`, and `0.18` seconds with identical
outputs. Floating-point conversion remains unchanged because it has different parsing and error
handling considerations.

The Deribit snapshot reader now uses the same `string_view` field path while constructing its typed
25-level snapshot vectors, while retaining explicit vector capacity reservation. Snapshot,
incremental, and orderbook tests passed; three same-window runs were `0.40`, `0.39`, `0.41` seconds
for snapshot input and `0.16`, `0.16`, `0.16` seconds for incremental input, with identical outputs
within each source. The snapshot short-run timing is neutral, but per-row string materialization is
removed from the snapshot parser.

Snapshot map construction now uses insertion hints because the validated input levels arrive in
the exact bid-descending/ask-ascending order required by the maps. L2 tests passed; on a same-window
short replay, snapshot runs took `0.12`, `0.12`, and `0.12` seconds while incremental runs took
`0.06`, `0.05`, and `0.05` seconds, with identical outputs. This optimization is specific to
ordered snapshot construction and does not change incremental update behavior.

The incremental reader now reuses its `string_view` field vector across batches and carries the
already-parsed batch/local timestamps into `append_update`, avoiding repeated integer conversions
for the same row. Three fixed-window runs took `0.14`, `0.14`, and `0.14` seconds with identical
outputs. This targets the long-profile hotspots `split_csv_views`, `parse_uint64`, and
`append_update` as one parser chain.

The L2OrderBook bid/ask maps now use an `unsynchronized_pool_resource` for map-node allocation,
including snapshot temporary maps and reset release. The ordered-map semantics remain unchanged;
this only changes node allocation/reuse. L2 tests passed, and the same short window measured
`0.05`, `0.06`, `0.05` seconds for incremental input and `0.13`, `0.12`, `0.13` seconds for
snapshot input, with identical outputs. Longer-window memory behavior still needs monitoring before
claiming the allocator change as final.

The one-hour no-output memory check reported maximum RSS of `5,464 KB` for incremental input and
`5,172 KB` for snapshot input. Wall times were approximately `0.87 s` and `2.40 s`, respectively;
these runs showed no abnormal pool growth for the measured workload.

The parser chain is now direct for incremental rows: a `ParsedRow` is filled by one column scan and
then converted to `IncrementalBookUpdate`, without constructing a per-row field vector at all. The
same row object also supplies batch grouping and typed conversion values. Adapter/replay tests
passed; three fixed-window runs took `0.15`, `0.15`, and `0.16` seconds with identical outputs.

The same checked `std::from_chars` approach is now used for ordinary floating-point fields, with
`std::stod` retained as a fallback for formats the fast parser rejects. Adapter/replay tests and
three fixed-window runs passed with identical outputs; the short runs were `0.21`, `0.21`, and
`0.21` seconds, so no standalone speedup is claimed yet.

The incremental reader now passes its known column count into the reusable CSV splitter, avoiding
an extra comma-counting scan on that path. Three fixed-window runs took `0.17`, `0.16`, and `0.15`
seconds with identical outputs. This result is still a short-run comparison, but the change keeps
the optimization local to incremental parsing and does not alter CSV field semantics.

Incremental batches now carry optional snapshot metadata computed while the reader appends updates.
The reader, validator, and `L2OrderBook` reuse that flag instead of rescanning every update list;
manually constructed batches without metadata retain the original scan fallback. The focused
tests passed and three fixed-window outputs remained identical (`0.16`, `0.24`, `0.16` seconds),
so the short timing is treated as noisy while the scan reduction is retained as the structural
benefit.

`IncrementalBookUpdate` no longer stores a duplicate `exchange` string: the exchange is already
owned by `IncrementalBookBatch`, and no downstream code consumed the per-update copy. The adapter,
orderbook, validator, and replay tests passed; three fixed-window runs took `0.15`, `0.14`, and
`0.15` seconds with identical outputs. This reduces the per-update object size and string-copy
work without changing the batch-level exchange metadata.

The L2 update path now uses `insert_or_assign` for non-zero price-level quantities, and the snapshot
reader reserves its known depth vector sizes before appending levels. The related tests passed. On
the same 15-minute window, three incremental runs took `0.14`, `0.15`, and `0.15` seconds; three
snapshot runs took `0.34`, `0.36`, and `0.36` seconds. Outputs were identical within each source.

The L2OrderBook price maps now use integer `PriceTick` keys configured with the instrument tick
size, converting to/from double only at the input/output boundary. This removes floating-point
ordering from the core map and makes price-level identity explicit. L2/orderbook/strategy tests
passed; the updated three-run sample was `0.15`, `0.15`, `0.16` seconds for incremental input and
`0.40`, `0.39`, `0.40` seconds for snapshot input. Incremental output remained identical; the
snapshot timing is slightly higher in this short sample, so this change is treated primarily as
a correctness/data-structure improvement, not a claimed universal speedup.

`MarketDataValidator::validate` now performs merged timestamp ordering, event counting, and
variant-specific validation in one `std::visit` instead of dispatching the same `MarketEvent`
twice. Validator/replay tests passed; three incremental runs took `0.16`, `0.16`, and `0.15`
seconds with identical outputs.

The L2 Pipeline now uses `insert_or_assign` for the latest quote map and order lifecycle maps,
avoiding default construction before assignment. Pipeline/accounting, replay, and benchmark tests
passed; three incremental runs took `0.14`, `0.14`, and `0.13` seconds with identical outputs.

`L2ReplayFeed::run` now caches the current trade and depth event timestamps while merging the two
streams, updating each cache only when its reader produces a new event. This avoids repeated
variant visits in the merge comparison. Replay/pipeline tests passed; three incremental runs took
`0.14`, `0.13`, and `0.13` seconds with identical outputs.

The incremental validator now keeps references to the per-symbol stream state and snapshot-active
state during one batch, avoiding repeated unordered-map lookups. Validator/replay tests passed;
the short timing sample was noisy (`0.41`, `0.35`, `0.46` seconds) but outputs remained identical,
so this is retained as a structural cleanup rather than a standalone speedup claim.

Validator stream names are now assembled only when an ordering error is thrown: normal events pass
a static stream type and symbol separately. Quote state also uses `insert_or_assign`. Validator and
replay tests passed; three fixed-window runs took `0.16`, `0.17`, and `0.17` seconds with identical
outputs.

The incremental path now parses rows through `string_view` fields into typed
`IncrementalBookUpdate` values instead of materializing a `vector<string>` for every row. The
per-update symbol was removed because symbol ownership belongs to the batch; the reader validates
the row symbol at construction time. `MarketDataValidationConfig` now exposes
`validate_incremental_update_fields`, defaulting to strict validation while allowing a replay
caller to skip duplicate per-update checks after the reader boundary has been trusted. Adapter,
L2, and pipeline tests passed; three fixed-window runs took `0.18`, `0.15`, and `0.17` seconds with
identical outputs.

Prepare the same slices and replay with:

```bash
profile_dir=$(mktemp -d)
.venv/bin/python - "$profile_dir" <<'PY'
import sys
from pathlib import Path

sys.path.insert(0, "tools")
from benchmark_l2_strategies import slice_csv

root = Path(sys.argv[1])
start_ts = 1585699200000000
end_ts = 1585700100000000
slice_csv(Path("data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz"),
          root / "trades.csv", start_ts, end_ts)
slice_csv(Path("data/v2/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv"),
          root / "depth.csv", start_ts, end_ts)
PY
/usr/bin/time -f '%e' ./out/build/linux-debug/toy_quant l2_replay \
  "$profile_dir/trades.csv" "$profile_dir/depth.csv" BTC-PERPETUAL \
  0 active_l2 1 conservative
```

`l2_replay` writes to the fixed `data/runtime/orders.csv`, `data/runtime/trades.csv`, and
`logs/toy_quant.log` paths. Preserve these files before replay if their current contents matter.

For a compute-focused run that does not touch those files or emit Logger output, append
`--no-output` after the queue model:

```bash
./out/build/linux-debug/toy_quant l2_replay \
  "$profile_dir/trades.csv" "$profile_dir/depth.csv" BTC-PERPETUAL \
  0 active_l2 1 conservative --no-output
```

This mode routes order/trade streams and logs to null sinks. The default file-producing behavior
is unchanged.

For repeatable timing, use the repository runner instead of assembling shell loops manually:

```bash
.venv/bin/python tools/measure_l2_baseline.py \
  --binary out/build/linux-debug/toy_quant \
  --trades data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz \
  --depth data/v2/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv \
  --symbol BTC-PERPETUAL \
  --start-ts 1585699200000000 --end-ts 1585702800000000 \
  --runs 5
```

The runner slices inputs into a temporary directory, defaults to `--no-output`, reports timing
statistics, and verifies that all captured stdout/stderr hashes match. Pass `--fast-validation` to
measure the validation-free comparison path, or the default output path only when output I/O is part of the
measurement.

Append `--fast-validation` to skip duplicate per-update incremental field checks after the reader
boundary, while retaining stream ordering and batch validation:

```bash
./out/build/linux-debug/toy_quant l2_replay \
  "$profile_dir/trades.csv" "$profile_dir/depth.csv" BTC-PERPETUAL \
  0 active_l2 1 conservative --no-output --fast-validation
```

On a short fixed window, strict and fast validation both measured `0.04–0.05 s` with identical
empty output streams. This separates the validation cost for future profiling; it is not a claim
that validation is currently the dominant bottleneck.

## gprof Profile Preparation

The opt-in CMake target `toy_quant_profile` builds the same L2 executable with `-pg` and is
excluded from ordinary builds. Build it with the CMake Tools target `toy_quant_profile`, then
reuse the prepared slices above and aggregate repeated runs:

For non-instrumented profiling with optimization and symbols, use the separate
`linux-relwithdebinfo` CMake preset. It writes to `out/build/linux-relwithdebinfo` and does not
alter the existing Debug build directory.

```bash
profile_prefix="$profile_dir/gmon"
for i in {1..50}; do
  GMON_OUT_PREFIX="$profile_prefix" ./out/build/linux-debug/toy_quant_profile l2_replay \
    "$profile_dir/trades.csv" "$profile_dir/depth.csv" BTC-PERPETUAL \
    0 active_l2 1 conservative > /dev/null 2>&1
done
gprof ./out/build/linux-debug/toy_quant_profile "$profile_prefix".* \
  > "$profile_dir/gprof.txt"
```

This 50-run aggregate collected about `4.07` CPU seconds of gprof samples. The leading application
functions were:

| Function | Sample share |
|---|---:|
| `L2OrderBook::apply_incremental_batch` | 11.3% |
| Incremental CSV split/group helpers | 8–10% each |
| `L2OrderBook::market_view` | 6.8% |
| `ActiveL2MarketMaker::on_l2_market_view` | 3.5% |

`_init` accounted for about 19.8%, largely reflecting that this aggregate starts a fresh process
for every run. Treat these percentages as hotspot candidates, not precise event-loop costs: gprof
adds instrumentation overhead, the sample is one short market window, and I/O/process startup are
included. The normal non-instrumented replay timing above remains the performance comparison
baseline.

## Paired One-Hour Baseline

For a larger workload, the same one-hour window was replayed three times per depth source with
`active_l2`, `conservative`, and no depth sampling:

- Window: `[1585699200000000, 1585702800000000]`
- Trades: 9,310 rows; SHA-256 `c678297c64974a6bab3fdd240e3dcb3e75fe32ec401a106d6fa926955320a45d`
- Incremental: 1,113,638 rows, grouped into 669,482 batches plus 9,310 trades; SHA-256 `a0732c30303b3eb2083775edb0c8fb51a5cb8062133fe60af67382d3e8bae4a8`
- Snapshot: 351,516 rows / 360,826 replay events; SHA-256 `28fca5d55c734cac633e18d2980654a25cd6741c82b44b5f3c622356e952c9d4`

| Depth source | Run times (seconds) | Median | Output consistency |
|---|---|---:|---|
| Incremental | 1.90, 1.77, 1.77 | 1.77 | All three captured outputs identical |
| Snapshot | 4.35, 4.10, 3.70 | 4.10 | All three captured outputs identical |

These are separate replay baselines, not expected to produce identical strategy metrics: snapshot
and incremental feeds have different event granularity and state-update semantics.

The 12-run gprof aggregate on the one-hour incremental input retained the same broad ranking as
the short-window profile:

| Function group | Sample share |
|---|---:|
| `L2OrderBook::apply_incremental_batch` | 15.2% |
| Incremental reader append/split/group functions | about 26% combined |
| `L2OrderBook::market_view` | 4.7% |
| `ActiveL2MarketMaker::on_l2_market_view` | 1.6% |
| `_init` and process startup | 18.8% |

The higher-resolution run strengthens the case that input parsing/grouping and L2 state
reconstruction deserve attention before strategy-specific work. The startup share remains a
profiling artifact of launching a new process for every sample.

The unified baseline runner was then used for the final three-run, one-hour no-output comparison:

| Source | Rows | Seconds | Median | Output hash |
|---|---:|---|---:|---|
| Incremental | 1,113,638 | 0.894515, 0.875006, 0.839258 | 0.875006 | identical |
| Snapshot | 351,516 | 2.311291, 2.163872, 2.144108 | 2.163872 | identical |

These numbers are the current compute-only baseline for future L2OrderBook changes. They exclude
runtime CSV/log output and use the strict default validation mode.

## L2 Strategy Regression Matrix

All four retained L2 strategies are replayed on the same 15-minute window for each depth source,
with the conservative queue model. Window `[1585699200000000, 1585700100000000]`: 1,354 trade rows,
201,130 incremental depth rows, 54,170 snapshot depth rows, `quantity_scale=1`. Net PnL is
`equity - 1000`, i.e. the change in equity from the initial cash.

| Strategy | Incremental orders / fills / net PnL | Snapshot orders / fills / net PnL |
|---|---:|---:|
| `passive_l2` | 657 / 2 / -0.020 | 641 / 2 / -0.020 |
| `inventory_aware_l2` | 419 / 0 / 0.000 | 407 / 2 / -0.007 |
| `flow_aware_l2` | 417 / 0 / 0.000 | 363 / 1 / -0.033 |
| `active_l2` | 414 / 0 / 0.000 | 396 / 0 / 0.000 |

**These values supersede an earlier table that reported net PnL between -13 and -21.** That table was
produced while the fee model charged `price * quantity` on a Deribit contract, which is about 630
times the contract's 10 USD face value. Those figures were dominated by the overcharge rather than
by trading, so they said nothing about the strategies. `ARCHITECTURE.md` documents the corrected
model.

### Why the fill counts are near zero

Fees do not influence matching, so the corrected fee model leaves fills unchanged. Two other facts
explain the counts, and both are model behaviour rather than defects:

- The strategies cancel nearly everything they submit. In this window `passive_l2` submits 657
  orders and cancels 643 of them, while `active_l2` submits 414 and cancels 412. Cancelling and
  re-quoting at the same price loses the queue position and rejoins at the back, so the order never
  reaches the front.
- Under the default `conservative` queue model the queue advances only through real trades. A real
  queue at the touch mostly drains by cancellation, so requiring the whole displayed size to trade
  at your exact price makes a fill close to impossible.

The queue model is the lever, and the same window shows its spread. This table is generated, not
transcribed: the command below it runs every variant, prints the table, and wraps it in a block that
`tools/check_docs.py` reads back, so these numbers fail a check when they stop being true.

<!-- toyquant:check
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 passive_l2 1 conservative --fast-validation
then: submitted_orders=647 trade_reports=4 fill_rate=0.00618238 queue_ahead_consumed=66230 captured_edge_per_unit_ticks=2.125 markout_per_unit_ticks=-1.25 markout_count=2 max_abs_inventory=1 realized_pnl=-0.0204946 equity=999.98 fees_paid=0.008
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 passive_l2 1 prorata --fast-validation
then: submitted_orders=652 trade_reports=4 fill_rate=0.00613497 queue_ahead_consumed=50976 captured_edge_per_unit_ticks=1.375 markout_per_unit_ticks=-1 markout_count=3 max_abs_inventory=1 realized_pnl=-0.0134928 equity=999.987 fees_paid=0.008
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 passive_l2 1 optimistic --fast-validation
then: submitted_orders=652 trade_reports=4 fill_rate=0.00613497 queue_ahead_consumed=50570 captured_edge_per_unit_ticks=1.375 markout_per_unit_ticks=-1 markout_count=3 max_abs_inventory=1 realized_pnl=-0.0134928 equity=999.987 fees_paid=0.008
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 conservative --fast-validation
then: submitted_orders=406 trade_reports=1 fill_rate=0.00246305 queue_ahead_consumed=55460 captured_edge_per_unit_ticks=2.5 markout_per_unit_ticks=0 markout_count=0 max_abs_inventory=1 realized_pnl=-0.002 equity=1000 fees_paid=0.002
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation
then: submitted_orders=381 trade_reports=3 fill_rate=0.00787402 queue_ahead_consumed=56647 captured_edge_per_unit_ticks=1.16667 markout_per_unit_ticks=-0.5 markout_count=2 max_abs_inventory=1 realized_pnl=-0.00678302 equity=999.995 fees_paid=0.006
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 optimistic --fast-validation
then: submitted_orders=279 trade_reports=7 fill_rate=0.0250896 queue_ahead_consumed=18020 captured_edge_per_unit_ticks=0.5 markout_per_unit_ticks=-0.5 markout_count=7 max_abs_inventory=1 realized_pnl=0.0189169 equity=999.988 fees_paid=0.014
| field | passive_l2 conservative | passive_l2 prorata | passive_l2 optimistic | active_l2 conservative | active_l2 prorata | active_l2 optimistic |
|---|---:|---:|---:|---:|---:|---:|
| `submitted_orders` | 647 | 652 | 652 | 406 | 381 | 279 |
| `trade_reports` | 4 | 4 | 4 | 1 | 3 | 7 |
| `fill_rate` | 0.00618238 | 0.00613497 | 0.00613497 | 0.00246305 | 0.00787402 | 0.0250896 |
| `queue_ahead_consumed` | 66230 | 50976 | 50570 | 55460 | 56647 | 18020 |
| `captured_edge_per_unit_ticks` | 2.125 | 1.375 | 1.375 | 2.5 | 1.16667 | 0.5 |
| `markout_per_unit_ticks` | -1.25 | -1 | -1 | 0 | -0.5 | -0.5 |
| `markout_count` | 2 | 3 | 3 | 0 | 2 | 7 |
| `max_abs_inventory` | 1 | 1 | 1 | 1 | 1 | 1 |
| `realized_pnl` | -0.0204946 | -0.0134928 | -0.0134928 | -0.002 | -0.00678302 | 0.0189169 |
| `equity` | 999.98 | 999.987 | 999.987 | 1000 | 999.995 | 999.988 |
| `fees_paid` | 0.008 | 0.008 | 0.008 | 0.002 | 0.006 | 0.014 |

`prorata` shrinks the queue ahead of a resting order by the fraction the displayed size fell;
`optimistic` treats the whole decrease that way. For `passive_l2`, which always rests at the touch
where the queue ahead starts equal to the displayed size, the two coincide, which is why those two
columns agree to the last digit. Neither column predicts live fills: they are the range that
aggregated L2 data leaves open. The only profitable cell is `optimistic` on `active_l2`, which is a
warning about the fill assumption rather than a result. The recorded values come from the 200,000-row
slice at default order sizing, and the full command is inside the marker above.

One scale note, corrected later. `L2MarketMakerConfig::order_size` was `1` and `inventory_limit` was
`1`, because `strategy_factory.cpp` derives both from the instrument's `quantity_scale`:

```cpp
order_size      = max(min_order_quantity, quantity_scale / 1000)
inventory_limit = max(1,                   quantity_scale / 10)
```

That derivation assumes `quantity_scale` encodes an economic scale. For Binance `BTCUSDT` it is
`1000000`, so the L1 workload runs with an order size of 1000 units and an inventory limit of 100000.
For Deribit the depth quantities are already contract counts, so the spec sets `quantity_scale = 1` and
the same derivation produces an order size of **one contract** and an inventory limit of **one
contract**: `10 * 0.5 / 6374 = 0.0008` USD per tick of move, so every net PnL was a few cents and
rounding dominated it. The L2 workload was running 10^6 below the L1 workload's scale, and the strategy
it was describing was not the strategy anyone would have chosen.

`--order-size=N` and `--inventory-limit=N` now override the derivation without changing any existing
command line, verified by the full-file comparison still matching byte for byte. With them the matrix
becomes about something:

| Run, `active_l2`, 200k slice | orders | quantity | fills | realised PnL |
|---|---:|---:|---:|---:|
| default (`order_size` 1), conservative | 406 | 406 | 1 | -0.002 |
| `--order-size=10 --inventory-limit=100`, conservative | 406 | 4060 | 1 | -0.02 |
| `--order-size=100 --inventory-limit=1000`, conservative | 406 | 40600 | 1 | -0.2 |
| default, prorata | 381 | 381 | 3 | -0.0068 |
| `--order-size=10 --inventory-limit=100`, prorata | 408 | 4080 | 3 | -0.0678 |
| `--order-size=10 --inventory-limit=100`, optimistic | 411 | 4110 | 7 | **-0.605** |

Two conclusions. **Order size scales the PnL linearly and never changes the fill count**, under any queue
model: `prorata` fills three times at the default size and three times at ten times the size, with ten
times the PnL, and `conservative` and `optimistic` keep their counts fixed the same way. What sets the
fill count is the queue model. And **at a realistic size the strategy loses more per fill in absolute
terms**, because an order of ten contracts moves the strategy's own inventory skew by ten times as
much. Three fills still prove nothing statistically, but they are now large enough to be about
something, which the previous scale could not express at all.

The size change also exposed a reporting gap, and three fixes went in with it. `[EXECUTION]` printed a
subset of `ExecutionQualityMetrics`, while `[STRATEGY_METRICS]` printed the same quantities from the
strategy's own copy, which only the legacy L1 maker fills; on the L2 path that copy stayed at zero, so
the line showed `captured_edge=0 adverse_selection=0 markout_count=0 max_abs_inventory=0` beside an open
marked position. `[EXECUTION]` now prints the authoritative values from the one place that computes
them. `max_abs_inventory` is now updated where the position changes rather than sampled from a market
view, which had missed a fill arriving as the last event, and `inventory_sign_changes` is counted there
too instead of never. And the markout queue is now drained on the L2 path, which only the L1 path had
done, so adverse selection had been structurally zero on this workload however often the strategy
traded. The legacy maker's positional `StrategyMetrics` initialiser became a named one in passing,
because inserting into that struct silently mis-assigned every field after the insertion point.

With those, the same run answers the question a market maker exists to answer. At
`--order-size=10 --inventory-limit=100` under `prorata` it reports `captured_edge=1.75`,
`adverse_selection=-0.5` and `markout_count=2` of 3 fills, so a third of the edge captured at the fill
is given back five cycles later. Under `conservative` it reports `captured_edge=1.25` and no resolved
markouts, because the one fill arrives too late in the run to have one. The numbers describe the fill
model as much as the strategy: `optimistic` reports `captured_edge=2.75`, `adverse_selection=-1.5` and
`markout_count=6` of 7 fills, which is what the removed `heuristic` produced as well.

Two modelling choices that the earlier notes called limitations are better closed with arithmetic than
with code, because their effect is bounded below anything a run here can measure:

- **Funding is not modelled.** A Deribit perpetual pays funding every eight hours, typically around
  0.01% of notional per interval. A position held for the seconds a maker holds one crosses a boundary
  with probability of order `hold/28800`, so the expected cost is about
  `0.0001 * notional * hold / 28800`. At 10 contracts, $100 of notional, held for a minute, that is
  roughly `2e-6` USD against fills worth about 0.08 USD each. Modelling it would add a rate input that
  is not in the data and whose effect is four orders of magnitude below the PnL it would adjust.
- **Collateral is cash only.** The engine holds no margin and no leverage, so exposure cannot exceed
  cash. That is not an approximation while `inventory_limit * unit_notional <= equity`: at
  `--inventory-limit=100` on a 10 USD contract the maximum position is 1000 USD against 1000 USD of
  starting cash, so the model sits exactly at the boundary where a margin model would begin to matter.
  Past that boundary a run would need one, rather than silently ignoring it.

These are single-run replay reference values, not profitability evidence. Repeat a row with the same
input hashes when checking exact output determinism.

### Queue position at arrival

A resting order is never first in the queue, so `displayed_quantity_ahead` returns the displayed best
size of the side the order joins. It previously returned 0 whenever the order price was not exactly
the best price, which let an order resting behind the touch fill in full the moment any trade printed
through its level. Those fills are selectively optimistic, because they appear exactly when the
market is moving against the resting side. Correcting this removed four of the six fills the window
previously produced; the two that remain are `passive_l2` quotes that sat at the touch, where the
old and new estimates agree.

### Calibrating the queue model against the feed

The fraction of a displayed-size decrease that a queue model honours was the last part of the fill
model resting on a number nobody had measured, so it was measured. `tools/calibrate_queue_model.py`
walks the incremental depth stream, tracks every price level, and attributes each fall in a level's
displayed size to the trades printed at that price on that side between the level's previous update
and that one. Whatever is left over is cancel or amendment.

Over the first 200,000 incremental depth rows, which span 898 seconds of 2020-04-01:

| Quantity | Value |
|---|---:|
| level decreases observed | 98,922 |
| total size removed from levels | 5,162,246,010 |
| explained by trades at the same price | 2,136,580 (0.04%) |
| therefore cancelled or amended | 5,160,109,430 (99.96%) |
| decrease events at least 99% cancellation | 98,268 (99.3%) |
| events where trades exceeded the decrease | 2 (0.0%) |

Two checks say the attribution is not an artefact of the window logic. The trade file holds 1,334
trades and 2,146,690 contracts inside that same 898 seconds, and the windows captured 2,136,580 of
them, or 99.5%, so essentially all of the traded volume is accounted for rather than falling between
updates. And only two of 98,922 decreases had more trade volume attributed to them than the level
actually lost, which is what a misalignment between the two files would produce in bulk.

So the displayed book churns **about two thousand four hundred times the volume that actually
trades**. On this instrument a level's size is mostly a quotation that is withdrawn and replaced: a
resting order's queue ahead of it is dissolved by cancellations far more often than it is consumed by
trades. That is what made the old ad-hoc `heuristic` untenable, since its two constants, a cap on the
reduction it honoured and then a fixed fraction of it, had no reading that matches this measurement,
and it is also why `conservative` should be read as a floor rather than as a neutral assumption.

What the data cannot show is *where* in a level's queue those cancellations sat, because that is not
public. `prorata` therefore assumes they are uniform and shrinks the queue ahead in proportion to the
displayed size, which the two measurable extremes bracket: nothing moves (`conservative`) or all of it
does (`optimistic`). The measurement narrowed the range rather than closing it, and the sensitivity
table above shows what is still at stake in it: three fills against seven on one window, and one
profitable cell out of six.

### Strategy execution correctness pass

Three defects in this area were one class of mistake: a number or a decision that two components each
owned, with nothing comparing them. They were fixed together, and the measurements below are from the
200,000-row depth slice.

**A wrapper that forgot one callback.** `ActiveL2MarketMaker` and `InventoryAwareL2MarketMaker` held an
`L2MarketMaker` and hand-forwarded each of its eight virtuals. `InventoryAwareL2MarketMaker` never
forwarded `on_queue_activity`, which silently disabled two things at once: the base's queue-consumption
counters, which reported zero, and the base's queue-hold refresh policy, which is built on the same
member. A forgotten forward changes behaviour without failing to compile, so both classes now derive
from `L2MarketMaker` and inherit everything they do not override. On this workload the refresh
behaviour barely moved (`price_refresh_count` 221 → 221, age 121 → 120) because the hold only binds at
a long quote age, but the counters went from `buy/sell_queue_consumed=0/0` to `1310/47480`.

**Two copies of every execution number.** `[STRATEGY_METRICS]` printed its own `captured_edge`,
`adverse_selection`, `markout_count`, quote lifetime and inventory fields beside `[EXECUTION]`, and
they disagreed: the same run reported `captured_edge=1.25` and `max_abs_inventory=10` in `[EXECUTION]`
and `0` and `0` in `[STRATEGY_METRICS]`, because only the legacy L1 maker ever filled those fields.
That is the same shape as the four-defect line fixed earlier; the earlier fix made `[EXECUTION]`
authoritative without removing the copy that contradicts it. `StrategyMetrics` now carries only what a
strategy owns (its own submit, cancel, fill, refresh and queue counters) and execution quality exists
once, in `ExecutionQualityMetrics`. The Python benchmark's parser already read only the strategy-owned
fields, so this split matches what the tools expected. The L1 maker still computes its own markout
internals, which its unit tests assert and nothing publishes; deleting that duplicate implementation is
the follow-up, not part of this pass.

**A documented constraint that nothing enforced or measured.** `docs/PERFORMANCE_HISTORY.md` states
that the model is cash only with no margin, so exposure cannot exceed cash. No component enforced it,
reported it, or noticed it. A strategy without an inventory limit (`naive`, which also never cancels)
reached a cash balance of `-120,940 USD` while `equity` still printed `+29%`, and nothing in the
summary said so. Two changes: the summary now reports `max_abs_exposure_usd` against
`starting_cash_usd` and counts fills that pushed the marked exposure past it, and `--max-position=N`
adds a pre-trade gate that counts the quantity already resting on the side an order adds to, because
those orders can still fill. The gate is off by default, so recorded runs are unaffected, and the
default Deribit path is inside it anyway (`max_abs_exposure_usd=100` against `1000`). With the gate on,
the same `naive` run goes from 122,794 fills and `cash=-120,940` to 43,714 fills and `cash=+1,924`.
Without it, the run now says `max_abs_exposure_usd=487,710` and `exposure_over_collateral_fills=122,493`
instead of saying nothing. The L1 BTCUSDT baseline is the same story at a smaller
scale: by the same measure its exposure reaches 4,575 USD against 1,000 USD of cash, and 1,673 of
its 2,277 fills pushed exposure past the collateral. An earlier version of this paragraph said
seventy times, because the exposure field was computed from raw instrument units while BTCUSDT
counts a millionth of a BTC, so every money figure on that path was a million times too large.
The quantity scale is now passed in and both paths report dollars; the L1 edge figure moved from
-335,733 to -0.336 USD, which is what the recorded -2.67 ticks per contract over 1.26 units
implies.

**A cross-check that had never been run.** The strategy, the pipeline and the portfolio each track the
position independently. The summary now compares the first two on every report and counts divergences.
On the L2 workload it reports `strategy_position_mismatches=0`, so the three agree, which is what makes
the strategy's own sizing trustworthy. If a callback were dropped again this counter is where it would
show, rather than in a PnL difference nobody can attribute.

**One gap left open on purpose.** `ActiveL2MarketMaker` compares `pause_after_ticks_per_second` against
a mid-price speed built from the time between views, and this feed's view gaps run from microseconds to
hundreds of milliseconds, so that speed measures the gap distribution rather than the market:
`min_elapsed_seconds=0.05` is above the 8.5 ms mean gap, so the floor always applies. Measured, the
halt fires 46 times at the field's value of 9.0 and 246 times at 50.0, and every value in between lands
in the same place. The threshold that would mean something is a tick range over a fixed time window,
which is a behaviour change to a risk control, and it is recorded here as a known gap rather than
half-changed: the field is renamed to say it is a per-second rate, the finding is written next to it,
and the 200,000-row invariants were re-verified unchanged after the rename.

### Making the demo's numbers readable and behaving

The principle for this pass was to stay close to the mechanism, keep the demo showing something, and
leave the results unremarkable. Four changes, all measured on the 200,000-row slice.

**Edge and markout are now per-contract and in ticks.** They were raw sums of price differences, so
`captured_edge=1.25` could not be read against the half-spread the quote sat at, and two runs with
different fill sizes were not comparable. The weighted sums behind them produce a quantity-weighted
average per contract, and the same for the markout five cycles later:

| Model | fills | captured ticks per contract | markout ticks per contract | kept |
|---|---:|---:|---:|---:|
| `conservative` | 1 | 2.50 | none resolved | — |
| `prorata` | 3 | 1.17 | -0.50 | 0.67 |
| `optimistic` | 7 | 0.79 | -0.50 | 0.29 |

The captured edge falls as the fill count rises, which is the shape adverse selection should have, and
the giveback is now a number rather than a shrug: 43% of what was captured under `prorata`, 63% under
`optimistic`. That ratio is what an edge-leak investigation needs to move.

**The `naive` baseline was not a strategy.** It submitted two orders on every update and cancelled
none, so every quote rested until the market traded through it: 234,576 orders and a 52% fill rate on
200,000 rows, with the position reaching -12,000 contracts and cash -120,940 USD. A quote that is never
cancelled is a free option. It now refreshes on a mid move of one tick or a maximum quote age, the two
triggers a real maker uses:

| | before | after |
|---|---:|---:|
| submitted orders | 234,576 | 11,482 |
| fills (rate) | 122,794 (52%) | 31 (0.27%) |
| max marked exposure | 487,710 USD | 200 USD |
| fills past the collateral | 122,493 | 0 |

**An unknown strategy name fails instead of silently measuring the default.** The factory used to fall
through to `OptimizedMarketMaker`, which is also how the `optimized` name itself was implemented, so a
typo or a new caller produced a run that looked fine while measuring something nobody asked for.
`optimized` now has an explicit branch and everything else throws.

**A tick-size bug the new metric found.** `total_quote_distance_ticks` was `4.8891e+08` on the L1 path
while `total_quote_distance` was `4889.1`, a ratio of exactly `1e5`: the replay path built its pipeline
without the instrument's tick size, so every tick-denominated figure was computed against the default
`1e-05` instead of `0.10`. Fixing that one argument moves `total_quote_distance_ticks` to `48,891`
(3.6 ticks per quote, which is what the maker quotes) and the new per-unit figures from `-26654.6` and
`51508.9` to `-2.67` and `5.15`. The pinned invariants did not move, because none of them is
tick-denominated, and the full-file reference was re-recorded through `tools/verify_l2.sh ref`.

**One change weighed and deliberately not made.** `conservative` is still the default queue model
because every recorded table, the verifier's pinned values and the reference hash are stated under it.
The measurement says `prorata` is the model the mechanism supports, and under it the default L2 run
reports `submitted_orders=381 trade_reports=3 queue_ahead_consumed=56647 realized_pnl=-0.00678`
instead of `406 / 1 / 55460 / -0.002`. Flipping it is four commands: the default in `AppConfig`, the
two defaults in `MatchingEngine`, the pinned values in `tools/verify_l2.sh`, then
`bash tools/verify_l2.sh ref`. It is recorded as a decision to take rather than taken halfway.

### What the fill count is made of

The L2 mainline fills a handful of times in 200,000 rows: three under the calibrated default and one
under the most skeptical bound. That number now has an explanation, and the explanation changes what
the next step should be.

First the arithmetic, over the same 898 seconds. The median size displayed at a level is 20,150
contracts, while the median volume that actually trades at a price across that whole window is 7,180.
An order joining the back of a median level therefore waits behind a queue 2.8 times larger than all
the trade at that price, so under `conservative`, which moves the queue ahead only when a trade prints, a fill is not
unlikely but out of reach. The
strategy is not being punished by a harsh model; it is standing in a queue that never clears.

That is why the quote's position relative to the touch decides the outcome, and the run now counts it:

| field | 1 tick (at touch) | 2 ticks (default) | 4 ticks | 1 tick + `prorata` | 1 tick + `optimistic` |
|---|---:|---:|---:|---:|---:|
| submitted orders | 346 | 406 | 408 | 271 | 268 |
| **fills** | **3** | 1 | 1 | **11** | **15** |
| quotes at the touch | **234** (68%) | 209 (51%) | **59** (14%) | 194 (72%) | 192 (72%) |
| captured ticks per contract | **0.67** | 2.50 | **4.50** | 0.41 | 0.40 |
| markout ticks per contract | -0.25 | none resolved | none resolved | -0.15 | -0.18 |
| realized PnL USD | -0.033 | -0.002 | -0.002 | -0.015 | **-0.088** |

Reproduce with `--base-spread-ticks=N`, which is what the first three columns vary.

Three things fall out of it.

**Quoting at the touch is a real lever and a bad one.** One tick instead of two triples the fill count,
1 to 3, because the share of quotes that join the touch rises from 51% to 68%. The captured edge falls
from 2.50 ticks to 0.67 over the same move and the markout turns negative, so the extra fills are worth
a quarter of the edge given up to get them.

**The queue model is still the larger lever.** At the touch, `prorata` fills eleven times against three
under `conservative`, and `optimistic` fifteen. Placement is worth 3x and the queue assumption 5x, and
they compound: the best cell is 15 fills at a 5.6% rate, the worst is 1 at 0.25%.

**More fills lose more money, and the fee is the reason.** Realized PnL falls as the fill count rises,
and the markout bounds adverse selection at -0.25 ticks, so the loss is not selection. It is the fee
against the unit. One BTC-PERPETUAL contract has a fixed 10 USD face value, so a half-USD tick move is
worth `10 * 0.5 / 6421 = 0.0008` USD per contract, while the maker fee on that same contract is
`10 * 0.0002 = 0.002` USD. Capturing four ticks pays it; capturing the 0.4 to 0.7 ticks a quote at the
touch actually earns does not, by a factor of about six. The two-tick default sits almost exactly at
break-even, which is why the recorded runs are flat rather than profitable.

The next step is therefore not to buy more fills. It is a choice between raising the capture per fill
and finding out whether the remaining fills are enough to measure, or changing the unit economics with
a longer horizon, a different instrument, or a venue whose fee is small against its tick. Both are
decisions about what the demo is for; neither is a code change, because the placement lever now exists.

### What the fill model assumes, and what a realistic one would need

The queue model is the part of this simulator a reader should distrust first, so here is what it
assumes and which of those assumptions the data can check.

**The spine, in order of how much is assumed.**

1. **Price-time priority.** A resting order fills after everything ahead of it at its price, in arrival
   order, and new size at the same price joins behind it. That is what the engine implements and what a
   FIFO venue does.
2. **Our order is invisible to us.** The displayed size comes from the venue, so it is other
   participants: a 10-contract quote is 0.05% of a median level here and does not move it.
3. **Trades consume the front, exactly.** A print of size `t` at our price removes `t` from the queue
   ahead and fills us once that queue is gone. Nothing is assumed here, it is arithmetic.
4. **Cancellations are the rest, and they are the only real assumption.** A level's displayed size
   falls by `r`; trades explain a measured 0.04% of that, so 99.96% is cancel or amendment. Where in
   the queue those cancels sit is not public. `ProRata` assumes they are spread uniformly, which
   shrinks the queue ahead in proportion to the display:

   ```
   queue_ahead -= queue_ahead * r / previous_displayed
   ```

   `Conservative` (nothing comes off) and `Optimistic` (all of it does) bracket that.

**So there is no free parameter to tune.** The cancel share is measured and the placement of those
cancels is bounded by the two extremes, which is why the honest presentation is a range across all
three and why the default is now the neutral member rather than the most skeptical one: a model that
advances the queue only when a trade prints is not conservative, it contradicts the measured mechanism
by three orders of magnitude.

**What a more realistic model would need, ordered by how much it would move the answers.**

1. **Cancel position, observed instead of assumed.** Only order-level data can settle it, and this feed
   does not carry order identity. The range stands until a source that does is used.
2. **Latency in milliseconds.** The engine counts cancellation delay in market events, not time. At this
   feed's rate an event is under a millisecond, so the model cannot express the case that costs a maker
   money: our cancel arriving after the market has moved.
3. **Our own order larger than the print.** The engine can carry a partial fill, but on this workload
   our 10-contract quotes are always smaller than the prints that reach them, so that path is not
   exercised.
4. **Repricing and time priority.** A venue that lets an existing order be modified keeps its place;
   this feed does not publish order identity, so whether an increase at our price is new size behind us
   or an amendment ahead of us cannot be measured here.

### The fill count is available; the edge is what is missing

The requote policy turned out to be the largest single lever, larger than quote placement and
comparable to the queue model itself:

| `--refresh-price-ticks` | 1 | 2 (default) | 4 | 8 | 16 |
|---|---:|---:|---:|---:|---:|
| submitted orders | 682 | 271 | 182 | 171 | 168 |
| **fills** | 3 | **11** | **21** | 23 | 23 |
| fill rate | 0.44% | 4.06% | 11.5% | 13.5% | 13.7% |
| captured edge USD | 0.0012 | 0.0035 | 0.0016 | 0.0047 | 0.0047 |
| fees paid USD | 0.006 | 0.022 | 0.042 | 0.046 | 0.046 |
| realized PnL USD | +0.015 | -0.015 | -0.109 | -0.147 | -0.147 |

All at the touch and under the calibrated model, with only the requote threshold changing. Holding a
quote longer instead of re-joining the back of the queue is worth a factor of two in fills between the
default and four ticks, and eight times between the shortest and the longest setting, saturating near
23. So the fills are there to be taken; the early fill counts were a symptom of a quote that never held
its place rather than of a market that could not reach it.

What is not there is an edge that pays for them. Fees rise with the fill count and the captured edge
does not, so every additional fill loses more: fees are 10x the captured edge at the long settings and
3x at the default, and the only positive cell in the table is the shortest requote setting with three
fills, which is a sample the markout immediately contradicts at -0.5 ticks. The conclusion is that the
next question is not how to fill more, it is what a maker can capture on this instrument that exceeds a
0.02% fee on a fixed 10 USD face value.

### The queue model, as arithmetic

Let a price level display `D` contracts, of which `Q` are ahead of our resting order, so our share of the
queue is `q = Q / D`. Two kinds of event change `Q`.

**Trades.** A print of size `t` at our price removes from the front: `Q <- max(0, Q - t)`, and the order
fills when `Q` reaches zero. Nothing is assumed here.

**Cancellations.** The display falls from `D` to `D - r` with no trade at that price. Which of those `r`
contracts were ahead of us is not in the data. Write the amount that leaves from ahead as a random
variable `X(r)`; a queue model is a claim about its distribution.

| model | `X(r)` | `E[X]` | `Var[X]` |
|---|---|---|---|
| `conservative` | `0` | `0` | `0` |
| `prorata` | `r * q` | `r * q` | `0` |
| `lumpy` | `(r / k) * Binomial(k, q)` | `r * q` | `r^2 q (1 - q) / k` |
| `optimistic` | `r` | `r` | `0` |

The columns after the first are what the models *mean*, and they separate two different uncertainties.
`conservative` and `optimistic` bracket the **mean**: they are the claims that none, or all, of the
fall came from ahead of us. `k` moves the **variance** without touching the mean, because a binomial
with `k` draws has mean `k q` and variance `k q (1 - q)`, and each draw carries `r / k`. So `k` is not a
free knob either; it has a physical reading. A cancellation removes whole orders, and the measurements
say how big those are: the median level decrease removes 74% of a median level, decreases run from 100
to 360,000 contracts, and the removed fraction is flat at 0.5-0.7 across four orders of magnitude of
level size. The neutral reading of a fall of `r` is therefore "one or a few orders left, each ahead of
us with probability `q`", which is `k = 1`, and `k -> infinity` is the deterministic pro-rata rule. A
level of `n` equal orders would give `k = n r / D`, so `k` is L2-blind in the same way the placement of
the cancels is.

**What the measurement says, including a negative result.** The same window, quoted at the touch, with
a four-tick requote threshold, ten seeds per setting:

| model | fills over ten seeds | mean | range |
|---|---|---:|---|
| `prorata` | 21, 21, 21 | 21.0 | 0 |
| `lumpy`, `k = 1` | 17 17 19 19 19 17 21 19 17 21 | 18.6 | 17-21 |
| `lumpy`, `k = 4` | 19 19 19 19 21 19 21 21 19 21 | 19.8 | 19-21 |
| `lumpy`, `k = 16` | 21 19 21 19 19 19 19 19 19 17 | 19.4 | 17-21 |

**The fill count is largely insensitive to `k`.** The per-event variance is real, but a quote lives
through many level updates before it can fill, so most of that variance averages out along the way and
every setting lands within about 10% of the deterministic value. Lumpiness therefore changes *which*
quotes fill and *when*, which a count per window cannot see, and it does not change how many a window
produces. That is the honest reason it is offered as an alternative reading rather than as the default:
it buys mechanism, not magnitude.

**And "mean-preserving" is per event, not per run.** `E[X] = r q` holds by construction for every
event, but the fill count is a nonlinear function of the whole queue path, so a path that empties a
queue in one jump does not fill exactly as often as one that empties it smoothly. The 18.6 against 21
is that effect rather than a defect, and it is the reason the two runs are not reported as agreeing.

**Where the headline number actually comes from.** The paragraph here used to say that `q` inside
`E[X] = r q` is what moves the headline, and that the arrival rule is what sets `q`. Measuring it shows
that this is backwards, and the reason is worth the space. With `q` fixed at arrival, every cancel
removes `r q` from the queue ahead while the display falls by `r`, so the ratio is preserved:

```
q' = (Q - r q) / (D - r) = q (D - r) / (D - r) = q
```

`q` is therefore a fixed point of the pro-rata dynamics and the starting share barely matters. Only two
things break that fixed point: a trade at our price, which removes from the front rather than in
proportion, and a model that lets `q` drift for its own reasons. Across the arrival share, at the touch
with a four-tick requote threshold:

| `--arrival-share` | 1.0 | 0.75 | 0.5 | 0.25 | 0.0 |
|---|---:|---:|---:|---:|---:|
| fills | 21 | 21 | 21 | 23 | 35 |
| fill rate | 11.5% | 11.5% | 11.5% | 12.8% | 19.4% |

Starting at the back of the queue and starting halfway through it are the same run, and starting at the
front is worth less than a factor of two. Set against the levers measured around it, requote policy is
7.7x and the queue model 5x. The decisive modelling choice is therefore not where we arrive; it is
whether a cancellation is allowed to help us at all, which is exactly the mean of `X(r)`.
`conservative` says no and fills once, `pro-rata` says proportionally and fills twenty-one times, and
`optimistic` lets `q` drift to zero and fills more than either. The arrival share and the chunk count
are second-order on this book because trades, the only thing that breaks the fixed point, are 0.04% of
what moves a level.

### What entry latency costs, and what it does not

The engine takes `--order-latency-us`, which holds an order for that long on the exchange clock before it
exists in the book. Zero is the synchronous behaviour every other recorded number uses, and the mode is
exercised by a unit test that sends an order at one timestamp and checks it is not resting until an event
whose clock has passed the delivery time.

Quoted at the touch with a four-tick requote threshold, under the calibrated model:

| latency us | fills | fill rate | captured ticks | markout ticks | fees USD | realized USD |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 21 | 11.5% | 0.095 | +0.095 | 0.042 | -0.109 |
| 100 | 21 | 11.5% | 0.095 | +0.095 | 0.042 | -0.109 |
| 1000 | 21 | 11.5% | 0.095 | +0.095 | 0.042 | -0.109 |
| 5000 | 11 | 15.1% | 4.23 | +0.15 | 0.022 | -0.008 |
| 20000 | 11 | 16.9% | 4.09 | +0.45 | 0.025 | -0.016 |
| 50000 | 10 | 20.4% | 8.45 | -0.06 | 0.023 | -0.017 |

The first thing to read is a check rather than a result. **Sub-millisecond latency changes nothing at
all**, because this feed's views arrive every 8.5 ms on average, so an order that lands before the next
view lands in the book the strategy was looking at. The sweep is flat from 0 to 1 ms for that reason and
not because the parameter is inert.

The second is the mechanism, and it points the opposite way to the usual warning. **Entry latency makes a
quote stale in the favourable direction.** The strategy picks a price from the view at T, the order lands
at T + L at that now-stale price, and a fill there is a purchase below the current mid. The capture per
contract rises from 0.095 ticks to 8.45 and the fill count falls from 21 to 10: being slow at entry is
equivalent to quoting wider, and the fee per fill falls with the fill count.

What the table therefore does not contain is the leg that costs a market maker money, which is the
**cancel**. A late entry delays a quote that would have been good; a late cancel leaves a quote exposed
after the market has moved against it, and that is where adverse selection enters. Cancellation delay is
still counted in market events rather than in microseconds, so the classic latency cost is not modelled
yet and these rows say nothing about a venue where cancels are slow.

None of the rows supports a PnL conclusion: all six are below the thirty fills the project requires
before a PnL statement, which is the case the gate exists for.

### What a late cancel costs, and why the sign flips

The other half of the lifecycle is `--cancel-latency-us`, the delay between the strategy asking to cancel
and the exchange applying it. Same window, same policy, same model as the table above:

| cancel latency us | fills | fill rate | captured ticks | markout ticks | edge USD | realized USD |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 21 | 11.5% | +0.095 | +0.095 | +0.002 | -0.109 |
| 1000 | 27 | 16.9% | -0.315 | +0.574 | -0.007 | -0.130 |
| 5000 | 25 | 16.9% | -0.480 | +0.820 | -0.009 | -0.123 |
| 20000 | 25 | 17.1% | -0.440 | +0.700 | -0.009 | -0.156 |
| 50000 | 23 | 19.3% | -0.196 | +0.435 | -0.004 | -0.158 |

Entry latency made the capture larger. Cancel latency makes it **negative**. The quote stands through
whatever the market did while the cancel was in flight, so the fills that arrive are the ones where a
trade came through a price the strategy had already decided to leave: buying above the mid, which is what
a negative capture means. The fill count rises rather than falls, 21 to 27, for the same reason, because
a quote that is never pulled is always there to be hit.

The markout column is worth reading carefully, because the cost does not appear where the classic story
puts it. The markout five cycles later is positive in every row, so the loss is not a delayed drift after
the fill: it is realised at the fill itself, in the price. Measured against the fee, the edge per contract
goes from barely positive to about half the fee, and realized PnL gets worse as the cancel gets slower,
while the entry-latency table got better in the same column. The two directions are opposite, and that
asymmetry is the whole of what latency does to a market maker: being slow to arrive costs you fills,
being slow to leave costs you money.

None of these rows supports a PnL conclusion either. The largest fill count is 27, still below the thirty
the project requires.

## Interpretation and Next Step



The wall-clock baselines include startup, parsing, simulation, and output I/O; they do not identify
event-level latency. The L1 sample is unsuitable for profitability conclusions, and none of these
numbers constitutes a low-latency performance claim.

Preparation is now complete for the first optimization pass: a paired one-hour performance
baseline, a repeated gprof profile, and a four-strategy regression matrix are recorded. Use
`active_l2` incremental replay as the primary performance target; retain snapshot and the other L2
strategies as compatibility checks. Keep inputs, build flags, and output summaries fixed so
performance changes can be compared without changing behavior.

## Long-Window Profile Result

The latest profile used the current `toy_quant_profile` binary, four one-hour incremental runs,
and `--no-output`, so file output was removed from the sample. The leading application hotspots
were:

| Function | Sample share |
|---|---:|
| `split_csv_views` | 17.3% |
| `L2OrderBook::apply_incremental_batch` | 16.2% |
| `parse_uint64` | 12.1% |
| `DeribitIncrementalBookReader::append_update` | 6.9% |
| `L2OrderBook::market_view` | 6.4% |
| `ActiveL2MarketMaker::on_l2_market_view` | 2.9% |

This changes the next optimization priority: the parser and typed batch/update path are the main
cost centers, followed by L2 map updates. Strategy code is not currently a primary hotspot, and
output I/O was excluded from this profile. The percentages come from an instrumented Debug-style
gprof build and guide prioritization; they are not production latency numbers.

## Buffered Line Reader

The line-reading layer was replaced before any parsing work, because `std::getline` and `gzgets`
are both per-character loops. `src/market/line_reader.*` now pulls 1 MiB blocks from a plain
`std::ifstream` or a `gzFile`, locates line boundaries with `memchr`, and hands back a
`std::string_view` into the block, so the common case neither scans byte by byte nor allocates.
A line that crosses a block boundary is compacted to the front of the block before the next read,
so an owned copy is only needed for a line larger than one block. All five readers
(`BinanceAggTradeReader`, `BinanceBookTickerReader`, `DeribitBookSnapshotReader`,
`DeribitIncrementalBookReader`, `DeribitTradeReader`) use it, which also removed the
comma-counting pre-pass that the old `split_csv(std::string)` needed to size its reserve.

### Line layer in isolation

A standalone microbenchmark over the first 200 MB (2,591,303 rows, about 76 bytes per row) of
`data/v2/deribit_incremental_book_L2_2020-04-01_BTC-PERPETUAL.csv`, three runs, medians:

| Stage | Seconds | Throughput |
|---|---:|---:|
| Raw 1 MiB block reads only | 0.037 | 5.4 GB/s |
| Block reads plus `memchr` newline scan | 0.076 | 2.6 GB/s |
| `std::getline` into `std::string` | 0.115 | 1.7 GB/s |
| `LineReader::next_line` | **0.080** | **2.5 GB/s** |
| `LineReader` plus the current per-row `find(',')` field split | 0.200 | 1.0 GB/s |

Two conclusions matter more than the 1.44x on the line layer itself:

- `LineReader` is within 10% of the `memchr`-only floor, so the line layer is now close to the
  irreducible read cost. `std::getline` was adding about as much time as the scan itself.
- **Per-row field splitting costs 0.120 s, which is more than the entire pre-change line layer.**
  Eight `find(',')` calls over roughly 10 byte fields are dominated by `memchr` call setup rather
  than by bytes scanned. Within this layer the field split, not the line split, is the next target.
  The stage timings below show that the field split is in fact only a small part of the parser's
  total cost, so this isolation result should not be read as the overall priority.

  **Corrected later:** the `memchr` half of that reasoning is wrong. A byte loop measured the same
  speed as `std::string_view::find`, so this host's `memchr` is not slow for ten byte fields, and the
  only variant that beat it was a word-at-a-time scan, by about 12% — below the measurement floor. See
  [Low-Latency Design](LATENCY_DESIGN.md#a-word-at-a-time-scan-for-the-field-split).

### End-to-end

Workload: the full-day `data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz` (119,238 rows)
replayed against the first 2,000,000 rows of the incremental depth file, `active_l2` and the
conservative queue model. Runtime summary: 1,303,131 events, 1,183,894 incremental batches, one
snapshot batch.

| Build | Median | Range | Paired mean difference |
|---|---:|---|---:|
| Pre-change, with output | 1.52 | 1.39–1.77 | |
| `LineReader`, with output | 1.41 | 1.21–1.64 | **−0.156 s (−10.2%)** |
| Pre-change, `--no-output --fast-validation` | 1.61 | 1.46–2.02 | |
| `LineReader`, `--no-output --fast-validation` | 1.41 | 1.28–1.63 | **−0.258 s (−16.0%)** |

Nine paired runs each, alternating pre-change and `LineReader` so that thermal and system drift
affect both builds equally. **Only the paired difference is trustworthy here.** Unpaired runs of a
single build ranged over 0.38 s, which is larger than the difference between the builds, so an
earlier unpaired comparison on this host reported a misleading `1.46x`.

Behavior is unchanged: `data/runtime/orders.csv`
(`521275515d80052d84f5ffb458cc2b82311168d7d3c7002fdd5d54889540b835`) and `data/runtime/trades.csv`
(`171bddd11a1d60e42ff250b9a0e6b3bedc90a44325d676b88c1303f20b88a02c`) match the pre-change build
byte for byte, and the full stdout/stderr summary is identical.

### Where the end-to-end saving actually came from

The saving is larger than the isolation table predicts, and most of it is not the vectorised scan:

| Source | Estimated share |
|---|---|
| Removing one `malloc`/`free` pair per batch | largest |
| Per-character line scan removal (the isolation table) | about 27 ms of 258 ms |
| Per-row `std::string`/`vector<std::string>` materialisation in `DeribitTradeReader` and the two Binance readers, plus their comma-counting reserve pre-pass | small on this workload |

`read_batch` used `pending_row_ = std::move(row)`. The move steals the row buffer, so the next
`std::getline` has to allocate a fresh one: on this workload that is roughly 1.18M allocate/free
pairs. `pending_row_.assign(view)` reuses the existing capacity instead, so the allocation
disappears. This is worth recording because the profiling story pointed at parsing, and the
largest single win turned out to be allocation churn on the batching boundary.


### Coverage added

`tests/line_reader_test.cpp` covers LF, CRLF, empty lines, a missing trailing newline, a line
larger than one block, block level batching, gzip parity, and line-for-line parity against
`data/v2/test_aggTrades_5k.csv`. `tests/market_data_adapter_test.cpp` gained incremental book
reader coverage for `local_timestamp` batching, the snapshot batch that starts the stream, CRLF
rows, and the header sniffing in `make_deribit_depth_reader`.

Every test source now starts with `#ifdef NDEBUG / #undef NDEBUG / #endif` before its first
include. Without it `linux-relwithdebinfo` compiled the checks out, so a guarded setup call was
skipped while the following `std::get<BboQuote>` still ran and aborted with
`std::bad_variant_access`. That preset is the profiling build, so its test run has to mean
something; the whole suite now executes its checks in both presets.


## Stage Timing (before the container change)

The function level profile above left 38.2% of the sample unattributed, and its percentages are
shaped by the instrumentation itself: gprof adds a call at every function entry, so tiny helpers
are charged for the measurement and inlined helpers are charged to their caller. The stage timings
measure declared regions instead, which is the question that matters here: how long is each step of
turning a row into a quote.

`l2_replay ... --profile-stages[=N]` records eight regions on one event in every `N` (default 64)
and prints exact percentiles to stdout, so it also works together with `--no-output`. With the flag
absent the profiler is not attached and the run is unchanged; that was verified by diffing the full
stdout and the runtime CSV hashes against the pre-change binary.

### Clock cost on this host

Reading the monotonic clock is not the ~20 ns that is normally assumed here:

| Call | ns |
|---|---:|
| `std::chrono::steady_clock::now()` | 71 |
| `clock_gettime(CLOCK_MONOTONIC)` | 72 |
| `clock_gettime(CLOCK_MONOTONIC_RAW)` | 68 |

This host reports itself as WSL2, where these reads are not on a fast path. Eight regions need
sixteen reads per sampled event, about 1.1 microseconds, so timing every event would have cost
more than the code being measured. At an interval of 64 the added wall time is roughly 17 ns per
event, under 1.5% of a 1.3 microsecond event, which is why the instrumented and uninstrumented
runs take the same time.

Because the read cost is significant, the profiler calibrates its own instrument first: it probes
two back to back clock reads at construction and subtracts the minimum from every sample (35-44 ns
here). Without that subtraction each of the eight regions was charged for one clock read, which
inflated the column sum by roughly 400 ns per event and made it appear larger than the total run
time.

### Measured breakdown

Full-day Deribit trades (119,238 rows) against the first 2,000,000 rows of the incremental depth
file, `active_l2` and the conservative queue model, `--no-output --fast-validation
--profile-stages`. 1,303,131 events, 1,183,894 incremental batches, wall clock 1.80-1.85 s, which
is 1381-1420 ns per event.

| Stage | Samples | Share | Mean | p50 | p90 | p99 | p999 | Max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| read+parse | 20361 | **50.4%** | 922.4 | 622 | 766 | 1102 | 58996 | 2835896 |
| validate | 20361 | 5.0% | 85.1 | 62 | 122 | 192 | 860 | 61219 |
| book-apply | 18514 | 15.7% | 287.0 | 193 | 338 | 710 | 3312 | 559972 |
| market-view | 18514 | 9.0% | 145.2 | 111 | 180 | 299 | 1173 | 226713 |
| top-booking | 18514 | 1.9% | 44.3 | 23 | 75 | 125 | 405 | 42885 |
| engine | 18514 | 13.5% | 274.3 | 167 | 354 | 606 | 4569 | 233840 |
| queue-acct | 18514 | 0.6% | 16.7 | 7 | 43 | 76 | 173 | 43463 |
| strategy | 18514 | 3.9% | 122.7 | 48 | 114 | 307 | 7029 | 339400 |
| **column sum** | | 100.0% | 1897.7 | **1233** | | | | |

Three consecutive runs produced the same ordering and the same shares within one percentage point,
so the ranking is stable even though the absolute values track the machine state.

### How to read it

- Share is computed from the p50 column. The mean is not usable for stages that have rare stalls:
  `read+parse` reaches 39-59 microseconds at p999 and 2.8-9.3 milliseconds at its maximum, which is
  a page fault or a 1 MiB block refill showing up in a single sample. Those few samples move the
  mean by hundreds of nanoseconds while leaving the median where it belongs.
- The p50 column sum lands within 10-15% of the per-event wall clock time, so the eight regions do
  cover essentially the whole event. Treat the column sum as a ranking tool and the wall clock as
  the absolute figure.
- `top-booking`, `engine` and `queue-acct` had no row at all in the function level profile. They
  were previously part of the unattributed 38.2%.

### What this changes for the next step

`read+parse` alone is half of the per-event cost. Combining this with the isolation measurement
above, and knowing that a batch here holds one or two rows of about 76 bytes, the split inside one
batch is roughly:

| Part | ns |
|---|---:|
| line reader, two rows | 76 |
| field split, two rows at about 46 ns | 92 |
| number conversion, two rows at about 200 ns | 400 |
| batch assembly, including one `updates` vector allocation | 60 |

**Number conversion is the largest single item in the whole run, near a third of the per-event
cost.** It is two `std::from_chars<double>` and three `parse_uint64` calls per row. This corrects
the priority that the isolation benchmark implied, because that benchmark could only see the line
layer and pointed at field splitting, which is about 7% of the event.

That prediction was right, and acting on it took longer than it should have. Both halves — the
`from_chars<double>` for the price and a hand written integer path for the timestamps — were attacked
and rejected on isolation comparisons that under-measured them. Both were eventually reimplemented as
exactly equivalent fast paths, verified byte-identical on the full file, and accepted: together about
20% of `read+parse`. See [Low-Latency Design section 5](LATENCY_DESIGN.md#5-the-number-conversions) and
the [ledger](PERFORMANCE.md#5-optimization-ledger).

Updated order of work:

1. Number conversion inside the reader: fixed point parsing so that prices become integer ticks
   directly, plus a hand written integer path.
2. `book-apply` and `market-view`, together 25%.
3. `engine`, 13%, which is a larger target than the function level profile suggested.

Validation and strategy are together under 9%, so they stay out of scope for now.

Where that work stands, as recorded at the time: the reader keeps small batches inline and dispatches
columns through a jump table, and a fixed point rewrite of the price parse was measured and dropped
because it did not pay. `book-apply` and `market-view` were later rebuilt on `TickLadder`, and the
engine's per-symbol maps were later re-keyed by integer id. Both are in the
[optimization ledger](PERFORMANCE.md#5-optimization-ledger), with their mechanisms in
[Low-Latency Design](LATENCY_DESIGN.md).

## Queue Distribution Over Orders

The one-order trace showed a quote joining a 7,200 contract queue and being cancelled with 169 contracts
still in front of it. That was an illustration, and the claim it supports is about every order rather than
about one, so the pipeline now profiles each order that rests: the queue in front of it when it rests, the
closest that queue came while the order was alive, and the queue remaining when the order left. Five summary
fields carry the aggregate. On the mainline window, quoting one tick wide and re-quoting after a four-tick
move:

| Field | Value | Meaning |
|---|---:|---|
| `queue_profile_orders` | 182 | Orders that rested and died, which is every submitted order in this window |
| `queue_zero_orders` | 21 | Orders whose queue in front reached zero at some point |
| `queue_min_fraction_p50` | 0.961474 | Median closest approach, as a fraction of the queue joined behind |
| `queue_min_fraction_p10` | 0 | Tenth percentile of the same ratio |
| `queue_end_fraction_p50` | 0.961474 | Median queue still in front when the order left |

The median quote never came within four percent of the front, and the tenth percentile is zero, which is the
21 fills and nothing else: on this window no order reached the front of its level and went unfilled, so being
first in line was sufficient to trade and the fill rate is a statement about how long the strategy waits
rather than about where it quotes.

Two implementation facts are load-bearing, and both are covered by the pipeline test. The end-of-life queue
is read **before** a cancel is issued rather than from the `Cancelled` report, because the engine erases a
price level once no order rests in it: a reading taken after the order leaves would report an empty queue for
a level that was not empty, and the two percentiles above would both collapse to zero. And a fill sets the
closest approach to zero by construction, because a fill is what happens when the queue reaches zero, which
is why `queue_zero_orders` can never be smaller than the fill count.

The measurement is cheap: the audit map holds open orders only, so sampling every level we are resting in
costs a handful of map lookups per event, on a stage that is 0.5% of the event already.

**This changed the default summary, and the reference hash was re-recorded.** The five fields are printed
unconditionally, so `tools/verify_l2.sh full` failed until the reference was refreshed. The re-record was
taken deliberately and justified by proving the difference was exactly those fields: stripping them from the
new output and hashing the remainder reproduces the previous reference, `079a955a`, byte for byte over the
1.5 GB stream, and the tier 1 invariants on both the L2 and the L1 paths are unchanged. The only new
interaction with the engine is a `const` accessor, so no run can behave differently because of this. A reader
holding an older cached reference should run `tools/verify_l2.sh ref` once; the file lives in `/tmp` and is
not tracked, which is why the change is recorded here rather than in the diff.
