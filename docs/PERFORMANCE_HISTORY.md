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

### The Fair Comparison, and the Second Venue

Widening a quote and refusing a narrow book both cut the fills, so comparing them head to head means comparing them
at the same participation. On 2020-04-01, with the same slice and the same policy except for the mechanism:

| condition | submitted | fills | fees USD | realized USD | USD per fill |
|---|---:|---:|---:|---:|---:|
| no condition | 1146 | 146 | 0.292 | -0.757 | -0.00519 |
| refuse a book narrower than two ticks | 737 | 35 | 0.070 | -0.097 | -0.00276 |
| refuse narrow, plus fee cover | 372 | 12 | 0.024 | +0.00085 | +0.00007 |
| quote five ticks out | 1789 | 6 | 0.012 | +0.00160 | +0.00027 |
| quote eight ticks out | 1800 | 2 | 0.004 | **+0.00955** | +0.00478 |
| quote twelve ticks out | 1800 | 2 | 0.004 | **+0.01593** | +0.00797 |

At matched participation, quoting further out is better per fill than refusing narrow books, which corrects the
earlier claim that widening only reduces participation. Both routes still end in a handful of fills, and the
positive totals rest on two to six fills each, so neither is an edge: the honest reading is that the loss disappears
by not trading rather than by trading well.

The second venue is the same conclusion at a different fee scale. The OKX archive is usable from the portal rather
than the API, so these rows are measured from a prepared slice and are not held by the gate:

| run | records | snapshots | orders | fills | fees USD | captured USD | realized USD |
|---|---:|---:|---:|---:|---:|---:|---:|
| 2026-01-07, one hour | 300k | 5 | 2085 | 1323 | 167.7 | 1.18 | -189.9 |
| 2026-01-07, full day | 6.33M | 96 | 37593 | 25534 | 3149.0 | 15.26 | -4674.4 |
| 2026-01-11, one hour | 300k | 6 | 321 | 135 | 28.4 | 0.11 | -30.5 |

The fee to captured ratio is 142x on one hour, 206x on the full day and 262x on the second day. It is larger here
than on Deribit because the tick is 0.1 on a price near 93000, which puts the fee at about 186 ticks against 2.55 on
the Deribit contract, which is the same mechanism at two scales rather than a second phenomenon.

The pre-trade gate already exists and was simply off, which is worth saying because a roadmap row claimed it was
missing. `--max-position` refuses an order whose projected position, including the quantity resting in other orders,
would exceed the limit. On the same OKX hour it does what it is for and it also shows the trap in its units:

| order size | max position | fills | refused | max exposure USD | equity |
|---:|---:|---:|---:|---:|---:|
| 100000 | none | 1323 | 0 | 18384 | 810.2 |
| 100000 | 20000 | 0 | 372660 | 0 | 1000.0 |
| 10000 | none | 505 | 0 | 10099 | 1014.8 |

The middle row is a limit smaller than a single order, so every order is refused and the run does nothing: loud in
the counter, silent in the output. The last row is the reason the gate matters. With a smaller order the same hour
ends positive, +13.89 USD, but the exposure reached ten times the starting cash, so what it earned was a favourable
move taken with unhedged size rather than anything the quotes captured. A result without the limit is not evidence
about a strategy.

With the limit on and an order size below it, the same hour behaves: exposure is held at 2,808 USD against the
1,000 USD it started with, the gate refuses 34,704 orders, and the equity ends at 973. The run also reproduces the
bucket split on the second venue, which had only ever been measured on Deribit:

| bucket | ticks | fills |
|---|---:|---:|
| buy, at least a tick from the midpoint | 110.2 | 97 |
| buy, at the midpoint | 44.4 | 121 |
| sell, at least a tick from the midpoint | 78.7 | 72 |
| sell, at the midpoint | 34.3 | 125 |

Fills away from the midpoint are worth two to three times the ones at it, on a venue with a different tick, price
and contract size, which is the strongest form the finding has taken.

<!-- toyquant:check
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --min-spread-ticks=2
then: submitted_orders=737 trade_reports=35 fees_paid=0.07 realized_pnl=-0.0967336
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=8 --refresh-price-ticks=4
then: submitted_orders=1800 trade_reports=2 fees_paid=0.004 realized_pnl=0.00954906
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=12 --refresh-price-ticks=4
then: submitted_orders=1800 trade_reports=2 fees_paid=0.004 realized_pnl=0.0159314
-->

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

#### The same measurement on an equity book

The calibration above is one instrument's, so it was repeated on the equity window the F4 results are measured
on: the converted SPY depth and trades pair for 2026-09-29 13:30 to 15:30, which is 2,593,706 depth rows and
24,298 trades.

| Quantity | Deribit BTC-PERPETUAL, 898 s | SPY, two hours |
|---|---:|---:|
| level decreases observed | 98,922 | 1,298,796 |
| total size removed from levels | 5,162,246,010 | 207,950,416 |
| explained by trades at the same price | 0.04% | **0.5%** |
| therefore cancelled or amended | 99.96% | **99.5%** |
| decrease events at least 99% cancellation | 99.3% | 98.7% |
| traded volume in the window | 2,146,690 | 1,544,421 |
| **book churn against traded volume** | **~2,400x** | **~135x** |

The same qualitative answer, with one substantive difference. Cancels still explain almost all of the size that
leaves a level, so a queue model that moves only when a trade prints is still a floor rather than a neutral
reading. But the equity book churns 135 times its traded volume against the perpetual's 2,400, so a quote's
queue is refreshed eighteen times less hard per share that trades, and that is what makes the queue model
readable here:

| Queue model | fills | filled shares | capture per fill, ticks | fees USD | net USD | peak inventory |
|---|---:|---:|---:|---:|---:|---:|
| `conservative` | 799 | 33,352 | 0.267855 | 116.73 | 262.51 | 1,099 |
| `prorata` | 2,528 | 90,286 | 0.419556 | 316.00 | 396.14 | 1,095 |
| `lumpy` | 2,281 | 88,124 | 0.418853 | 308.43 | 14.93 | 1,092 |
| `optimistic` | 2,716 | 105,458 | 0.436415 | 369.10 | 155.51 | 1,096 |

The fill count moves by 3.4x across the model space and the capture per fill by only 1.6x, and **the sign is what
moves: a fill pays one side's commission of 0.35 ticks, so the floor's 0.267855 ticks cover 77% of what that fill
pays while the other three models' 0.4189 to 0.4364 cover 120 to 125% of it**. The equity window's quoting edge
therefore sits *on* the fee rather than under it, which is the opposite of the perpetuals -- their fee was 2.57
ticks against a capture of 0.088 -- and what decides the sign here is the queue model plus whether the position the
run carries happens to pay, not the fee. The F4 table's SPY row -- 799 fills and 0.267855 ticks -- is that row's own
conservative floor rather than the window's fill count, and at that floor the quoting edge alone is 27.40 USD short
of the commission while the window's net is +262.51, so most of that net is the position and not the quotes.

The spread of `net USD` across the four rows is not the quoting: all four end carrying about 1,100 shares into
the final mark, which is where 14.93 against 396.14 comes from. Read the capture and the fill count here, the way
the flatten-band section says to read a row that ends holding a position.

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

## Funding Rates as an Input

The carry line's second step, and the first time a rate series reached the ledger: a replay now pays or receives
funding on whatever position it holds when a settlement arrives. Two free sources publish the series and they
disagree about nearly everything except the sign convention, which is why both are recorded here: each
difference is the kind that shifts a day or a factor without failing.

| Source | Archive | What its file day is |
|---|---|---|
| OKX `module=3` (`tools/fetch_funding_rates.py --source okx`) | one CSV per day for **every** swap on the venue | midnight **UTC+8**: the file named `2026-01-07` opens at `2026-01-06 16:00` UTC |
| Binance Vision `fundingRate` | one CSV per symbol per **month**; there is no daily folder, unlike `premiumIndexKlines` | UTC, and each row declares its own interval (`8` hours for BTCUSDT) |

Two runs over these files answer different questions and both are recorded, because the funding line reports
two different things.

**A window inside a month, with no position yet.** The five minute Binance fixture the L1 path already
documents starts at `2024-03-30 00:00:00.014` UTC, which is *after* 88 settlements of the March file. The replay
applies every one of them at its first event, against an account that holds nothing, so the count and the money
have to disagree:

<!-- toyquant:check
run: replay data/v2/test_aggTrades_5k.csv data/v2/test_bookTicker_5k.csv BTCUSDT 0 l1 1000000 --funding-rates=data/v2/BTCUSDT-fundingRate-2024-03.csv
then: funding_points=93 funding_span_hours=736 funding_sum_rate=0.0314019 funding_annualized_rate=0.373751 funding_settlements=88 funding_flow_usd=0
-->

**Ten minutes around one settlement, with a position.** A Binance day file is cut on UTC midnight, which is
itself a settlement instant, so a window that begins with the day cannot show the mechanism. The slice therefore
starts at `2024-03-28 07:55` UTC and ends at `08:05`, which contains the 08:00 settlement after ten minutes of
quoting. The same command with and without the funding file is the A/B: the fills and the fees are identical and
the funding is the only difference in the account.

<!-- toyquant:check
run: replay /tmp/btc_trades_settle.csv /tmp/btc_bbo_settle.csv BTCUSDT 0 l1 1000000
then: events=78477 trade_reports=111 fees_paid=1.25542 realized_pnl=-1.37681 equity=998.122
run: replay /tmp/btc_trades_settle.csv /tmp/btc_bbo_settle.csv BTCUSDT 0 l1 1000000 --funding-rates=data/v2/BTCUSDT-fundingRate-2024-03.csv
then: events=78477 trade_reports=111 fees_paid=1.25542 funding_settlements=83 funding_flow_usd=0.226775 realized_pnl=-1.15004 equity=998.349
-->

What the two runs establish:

- **The flow is positive because the position was short.** The published rate is what longs pay, the accounting
  takes the rate a long earns, and the `l1` strategy was net short when the settlement arrived, so 0.226775 USD
  was paid to the account rather than charged to it. Realized PnL moved from -1.37681 to -1.15004, which is that
  amount exactly, and equity moved with it from 998.122 to 998.349.
- **83 of the 93 settlements were reached**: the 82 before the window, applied to an empty account, and the one
  inside it, applied to a position. The ten after the window are why `funding_points` and `funding_settlements`
  are printed side by side.
- **The month is the carry number the acceptance criterion needs.** 93 settlements over 736 hours sum to
  0.0314019 and annualize to 0.373751: a long holding BTCUSDT through March 2024 paid about 37% of its notional
  to stay long. That is the base a hedged position has to beat after its own fees, and no fee schedule in this
  repository is near it, which is the first statement the carry line makes that the market-making line could
  not.
- **The venue's settlement clock is not exact.** Several `calc_time` values end in `...000001`, so the eight hour
  interval holds to within a second rather than exactly. `tests/funding_rate_test.cpp` asserts that tolerance
  with the reason attached, because a strict equality would have been a red test about Binance's clock.
- **The OKX day boundary needs both files.** Its funding archive is cut on UTC+8, so a range cut on UTC spans two
  of them, and the book archive for `2026-01-07` opens exactly at the `00:00` UTC settlement that the funding
  file named `2026-01-07` also contains. A replay on one file therefore reaches the settlements up to its own
  start and none after it.

The reader costs nothing measurable: it reads 93 rows once, and the ten minute settlement window replays in the
same 0.2 s as the five minute fixture. Both runs leave every recorded number in this document untouched, because
the funding path is entered only through `--funding-rates` and a run without it prints exactly what it printed
before.

## The Acceptance Number

The carry line's criterion, in its own words: **annualized carry against every fee the position pays**, on real
data. Two instruments of the same asset on two venues, one long and one short, held over the span their funding
histories share, and opened and closed at the venues' published taker rates. Binance BTCUSDT and OKX
BTC-USDT-SWAP in March 2024, 10,000 USD a leg:

<!-- toyquant:check
run: carry data/v2/BTCUSDT-fundingRate-2024-03.csv data/v2/okx_funding_2024-03-01_2024-03-31_BTC-USDT-SWAP.csv --symbol-a=BTCUSDT --symbol-b=BTC-USDT-SWAP --notional-usd=10000
then: carry_gross_notional_usd=20000 carry_net_notional_usd=0 carry_window_hours=728 carry_settlements=182 carry_leg_a_funding_usd=-304.656 carry_leg_b_funding_usd=322.241 carry_funding_usd=17.5849 carry_fees_usd=18 carry_net_usd=-0.415093 carry_annualized_carry_usd=211.599 carry_annualized_return=0.0211599 carry_to_fees=11.7555 carry_breakeven_hours=745.184
run: carry data/v2/BTCUSDT-fundingRate-2024-03.csv data/v2/okx_funding_2024-03-01_2024-03-31_BTC-USDT-SWAP.csv --symbol-a=BTCUSDT --symbol-b=BTC-USDT-SWAP --notional-usd=10000 --long=b
then: carry_leg_a_funding_usd=304.656 carry_leg_b_funding_usd=-322.241 carry_funding_usd=-17.5849 carry_net_usd=-35.5849 carry_annualized_carry_usd=-211.599 carry_to_fees=-11.7555 carry_breakeven_hours=0
-->

What it says, and it is not one thing:

- **The carry is the difference between the two venues, and it is small against what flows.** 304.66 USD was paid
  on Binance and 322.24 received on OKX, so the hedge cancels 94.5% of the funding it takes on and leaves 17.58
  USD of actual carry on 10,000 USD a leg. Either of the gross figures is what a report that showed one leg's
  funding would invite a reader to quote, and neither of them is the carry.
- **The ratio is above one and the month still lost 0.415 USD.** The annualized carry is 211.60 USD against 18.00
  USD of fees, a ratio of 11.76; but the fees are paid once while the carry accrued over 728 hours, which is 17
  hours short of the 745 the position needed to pay for them. Both numbers are in the line because they answer
  different questions, and a reader who takes the ratio alone reports a winner that the same line's net calls a
  loser by 0.42 USD.
- **The direction is the whole result.** Held the other way round, the identical position pays 17.58 USD of
  funding instead of earning it and loses 35.58 USD against the same 18.00 USD of fees, with a negative ratio and
  no breakeven at all. The two legs are one position and which side of it is long is a decision; the venues'
  funding difference is the only margin, and it is 2.1% annualized rather than the 37% a single venue's funding
  suggests.
- **The fee binds here too, but not the way it bound the market-making line.** There, a fee was 2.57 ticks
  against 0.088 captured and no lever moved the ratio. Here the fee is 0.18% of the notional and the carry is a
  rate against it, so the holding period, a maker entry instead of a taker one, or a wider difference between the
  venues each change the answer: this position needs 31.0 days to pay for itself and was held for 30.3. A margin
  of half a day is the honest reading of a criterion that asks whether this line has more to say than the last
  one, and the answer is that it has a number where the other line had none, and that the number is thin.

The second run's fields are the first run's with the sign flipped. `data/README.md` records where the two funding
files come from, one command each, and both blocks are re-derived by `tools/check_docs.py` on every gate run.

## The Maker Fee Each Window Would Have Needed

The market-making line's binding claim has always been about the fee: at a one-tick spread the fee is 2.57 ticks
against 0.088 captured. That comparison assumes the loss *is* the fee, and a reader is entitled to the other half
of the question: which maker rate would have made these windows profitable, and is it one a venue offers? The
answer needs two numbers the summary did not report, the PnL before fees and the notional the fees were charged
on, and both are now in every run's `[PNL]` line.

`closed_pnl_usd` is what completed, so a position the run was still holding is not in it; `net_pnl_usd` adds that
position marked at the last price; the `gross_` pair adds the fees back to each, and a negative gross PnL says the
run lost money before the venue charged anything, which no fee tier can fix. `maker_notional_usd` turns that into
a rate: `breakeven_maker_rate` is the maker fee at which the window would have been exactly flat, and a negative
one means the venue would have had to pay a rebate instead.

`tools/fee_sweep.sh` runs the sweep and prints the table with the block that re-derives every number. The three
rates are the venue's own maker fee, zero, and a rebate half the size of the fee:

| window | maker rate | fills | net USD | gross USD | closed USD | gross/fill USD | breakeven rate |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0401 incremental 200k | 0.0002 | 21 | -0.123813 | -0.0818127 | -0.109283 | -0.00320398 | -0.000389584 |
| 0401 incremental 200k | 0 | 21 | -0.0818127 | -0.0818127 | -0.0672835 | -0.00320398 | -0.000389584 |
| 0401 incremental 200k | -0.0002 | 21 | -0.0398127 | -0.0818127 | -0.0252835 | -0.00320398 | -0.000389584 |
| 0401 snapshot 200k | 0.0002 | 146 | -0.757038 | -0.465038 | -0.757038 | -0.00318519 | -0.000318519 |
| 0401 snapshot 200k | 0 | 146 | -0.465038 | -0.465038 | -0.465038 | -0.00318519 | -0.000318519 |
| 0401 snapshot 200k | -0.0002 | 146 | -0.173038 | -0.465038 | -0.173038 | -0.00318519 | -0.000318519 |
| 0501 snapshot 200k | 0.0002 | 58 | -0.287468 | -0.171468 | -0.287468 | -0.00295634 | -0.000295634 |
| 0501 snapshot 200k | 0 | 58 | -0.171468 | -0.171468 | -0.171468 | -0.00295634 | -0.000295634 |
| 0501 snapshot 200k | -0.0002 | 58 | -0.055468 | -0.171468 | -0.055468 | -0.00295634 | -0.000295634 |
| 0401 incremental 2M | 0.0002 | 267 | -1.39317 | -0.859172 | -1.38723 | -0.00319563 | -0.000321787 |

<!-- toyquant:check
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=21 net_pnl_usd=-0.123813 gross_pnl_usd=-0.0818127 closed_pnl_usd=-0.109283 gross_closed_per_fill_usd=-0.00320398 breakeven_maker_rate=-0.000389584
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0
then: trade_reports=21 net_pnl_usd=-0.0818127 gross_pnl_usd=-0.0818127 closed_pnl_usd=-0.0672835 gross_closed_per_fill_usd=-0.00320398 breakeven_maker_rate=-0.000389584
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=-0.0002
then: trade_reports=21 net_pnl_usd=-0.0398127 gross_pnl_usd=-0.0818127 closed_pnl_usd=-0.0252835 gross_closed_per_fill_usd=-0.00320398 breakeven_maker_rate=-0.000389584
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=146 net_pnl_usd=-0.757038 gross_pnl_usd=-0.465038 closed_pnl_usd=-0.757038 gross_closed_per_fill_usd=-0.00318519 breakeven_maker_rate=-0.000318519
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0
then: trade_reports=146 net_pnl_usd=-0.465038 gross_pnl_usd=-0.465038 closed_pnl_usd=-0.465038 gross_closed_per_fill_usd=-0.00318519 breakeven_maker_rate=-0.000318519
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/snap_20200401_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=-0.0002
then: trade_reports=146 net_pnl_usd=-0.173038 gross_pnl_usd=-0.465038 closed_pnl_usd=-0.173038 gross_closed_per_fill_usd=-0.00318519 breakeven_maker_rate=-0.000318519
run: l2_replay data/v2/deribit_trades_2020-05-01_BTC-PERPETUAL.csv.gz /tmp/snap_0501_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=58 net_pnl_usd=-0.287468 gross_pnl_usd=-0.171468 closed_pnl_usd=-0.287468 gross_closed_per_fill_usd=-0.00295634 breakeven_maker_rate=-0.000295634
run: l2_replay data/v2/deribit_trades_2020-05-01_BTC-PERPETUAL.csv.gz /tmp/snap_0501_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0
then: trade_reports=58 net_pnl_usd=-0.171468 gross_pnl_usd=-0.171468 closed_pnl_usd=-0.171468 gross_closed_per_fill_usd=-0.00295634 breakeven_maker_rate=-0.000295634
run: l2_replay data/v2/deribit_trades_2020-05-01_BTC-PERPETUAL.csv.gz /tmp/snap_0501_200k.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=-0.0002
then: trade_reports=58 net_pnl_usd=-0.055468 gross_pnl_usd=-0.171468 closed_pnl_usd=-0.055468 gross_closed_per_fill_usd=-0.00295634 breakeven_maker_rate=-0.000295634
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=267 net_pnl_usd=-1.39317 gross_pnl_usd=-0.859172 closed_pnl_usd=-1.38723 gross_closed_per_fill_usd=-0.00319563 breakeven_maker_rate=-0.000321787
-->

Three things follow, and the first is not the one this line has been repeating:

- **The fee is not what loses the money at a tight quote.** Before any fee is charged, every window loses between
  0.00296 and 0.00320 USD per fill, which is three to four ticks a fill, against a capture of 0.1 to 0.3 ticks.
  `breakeven_maker_rate` is negative on all four windows, between -0.000296 and -0.000390, which says the venue
  would have had to *pay* a rebate larger than its own maker fee for these windows to break even. At zero maker
  fees the largest sample is still down 0.86 USD over 267 fills.
- **Widening the quote does not pass through a profitable middle.** On the largest window, asking for two ticks
  halves the loss per fill and asking for four reverses the improvement, while the capture rises from 0.088 to
  0.367 ticks: the fills that arrive at a wider quote are worse per fill, not better, which is what the markout
  buckets and the out-of-sample corner had already said in a different form.
- **The capture finally beats the cost at eight ticks, with one fill.** There the capture is 6.5 ticks and
  `breakeven_maker_rate` turns positive at +0.000515, above the venue's 0.0002, so the window is up 0.0051 USD —
  but nothing closed in it, so that gain is the mark on the position the run is still holding, and one fill is not
  evidence. The honest form of this row is that the door exists and its width is unknown.
- **So the constraint is the fill population, not the schedule.** A maker is profitable when the capture per fill
  covers the fee plus the adverse selection it suffers, and on this instrument that second term is three to four
  ticks at every width where fills actually happen. That is why the next step is a rule that refuses a quote
  unless the expected fill covers both, rather than a hunt for a cheaper venue: at 0.0032 USD of loss a fill, no
  fee schedule on earth makes the tight-quote population profitable.

| quote width | fills | captured ticks/fill | gross USD | gross/fill USD | breakeven rate |
|---:|---:|---:|---:|---:|---:|
| 1 tick | 267 | 0.088015 | -0.859172 | -0.00319563 | -0.000321787 |
| 2 ticks | 207 | 0.0966184 | -0.480513 | -0.00232323 | -0.000232132 |
| 4 ticks | 15 | 0.366667 | -0.0452864 | -0.00315111 | -0.000301909 |
| 8 ticks | 1 | 6.5 | 0.00514688 | 0 | 0.000514688 |

<!-- toyquant:check
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=2 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=207 captured_edge_per_unit_ticks=0.0966184 gross_pnl_usd=-0.480513 gross_closed_per_fill_usd=-0.00232323 breakeven_maker_rate=-0.000232132
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=4 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=15 captured_edge_per_unit_ticks=0.366667 gross_pnl_usd=-0.0452864 gross_closed_per_fill_usd=-0.00315111 breakeven_maker_rate=-0.000301909
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=8 --refresh-price-ticks=4 --maker-fee-rate=0.0002
then: trade_reports=1 captured_edge_per_unit_ticks=6.5 gross_pnl_usd=0.00514688 gross_closed_per_fill_usd=0 breakeven_maker_rate=0.000514688
-->

**The reference hash was re-recorded.** The `[PNL]` line is printed unconditionally and the `[Mode]` line now
echoes the maker rate, so `tools/verify_l2.sh full` failed until the reference was refreshed. The re-record is
justified by proving the change is exactly those two additions: stripping the `[PNL]` line and the
`maker_fee_rate=` token from the new output reproduces the previous reference `f95d1c98` byte for byte over the
1.5 GB stream. The new reference is `cfabcd88`, and a reader holding a cached one should run
`tools/verify_l2.sh ref` once.

## The Decision Layer Refuses, and the Position Is Not the Profit

F1 measured that at a tight quote the fee is not what loses the money: the gross PnL is already negative, so the
fills themselves are the cost. Two changes follow from that, and both came out differently from what they were
built for, which is why both are recorded.

### The adverse-selection gate

`--markout-cover` (l2_replay, active_l2) makes the decision layer require a quote to cover the maker fee **and**
what passive fills at that quote's distance from the midpoint have actually cost. The first version measured that
cost from the venue's own prints — every trade, resolved five cycles later, as if we had been resting on the
passive side — and the record is why the decision now uses our own fills instead. Both rows come from the same
run, whose `[GATE]` line reports them side by side:

| what is measured | buy near | buy far | sell near | sell far | samples |
|---|---:|---:|---:|---:|---:|
| the venue's prints, as if we had been passive | 0.0539353 | 1.05413 | 0.0305481 | -0.189796 | 12683 |
| our own fills | 0.333333 | -0.7 | 0.285714 | -0.75 | 17 |

A print at a distance and a quote at that distance being hit are not the same event, and on this window they
disagree in sign in the far bucket: the prints say being passive there earned a tick, our fills say it cost
seven tenths of one. **A measurement of the market is not a measurement of us**, which is why the gate is built
on our own fills, and why it waits for twenty of them before tightening at all — a gate that tightened from its
first decision would refuse everything and never learn anything.

With the gate on, on the two million record window and at the venue's own maker fee:

| quote width | fills | net USD | gross USD | breakeven rate | refusals |
|---:|---:|---:|---:|---:|---:|
| 1 tick | 21 | -0.0920398 | -0.0500398 | -0.000238285 | 9709 |
| 2 ticks | 17 | -0.302228 | -0.268228 | -0.00157781 | 0 |
| 4 ticks | 7 | **+0.0963399** | **+0.11034** | **+0.00157628** | 0 |

<!-- toyquant:check
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=1 --refresh-price-ticks=4 --markout-cover
then: trade_reports=21 net_pnl_usd=-0.0920398 gross_pnl_usd=-0.0500398 breakeven_maker_rate=-0.000238285 markout_gate_skips=9709
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=2 --refresh-price-ticks=4 --markout-cover
then: trade_reports=17 net_pnl_usd=-0.302228 gross_pnl_usd=-0.268228 breakeven_maker_rate=-0.00157781 fill_markout_buy_far_ticks=-0.7 fill_markout_sell_far_ticks=-0.75 fill_markout_samples=17 market_markout_buy_far_ticks=1.05413 market_markout_samples=12683
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_2m.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=4 --refresh-price-ticks=4 --markout-cover
then: trade_reports=7 net_pnl_usd=0.0963399 gross_pnl_usd=0.11034 breakeven_maker_rate=0.00157628 markout_gate_skips=0
-->

The ungated rows of the same window are in the F1 block above: 267 fills at one tick with a gross of -0.859172,
207 at two with -0.480513 and 15 at four with -0.0452864. Read together, the gate does two things and they are
worth separating:

- **Refusing the quotes that cannot pay is most of the improvement.** At one tick the fill count falls from 267 to
  21 and the gross loss from 0.859 to 0.050 USD, a seventeen-fold reduction in what that population loses, and the
  four-tick row turns positive with a breakeven rate eight times the venue's own fee.
- **The measured part is a refinement this sample cannot evaluate.** The gate's own success starves its estimator:
  the more it refuses, the fewer fills resolve, and seven to twenty-one fills is below the twenty the gate waits
  for. What is established here is the mechanism and its direction, not a number of its own.

### The flatten band

`--flatten-band=N` prices an order at the touch on the reducing side as soon as the position passes N units, so the
engine fills it as a taker instead of the position being carried into the final mark. On the two hundred thousand
record window, ten contracts a quote and a limit of forty:

| band | fills | max abs inventory | flatten orders | flatten quantity | taker fees | gross USD | net USD |
|---:|---:|---:|---:|---:|---:|---:|---:|
| off | 11 | 21 | 0 | 0 | 0 | **+0.315192** | **+0.107192** |
| 10 | 21 | 20 | 3 | 21 | 0.105 | -0.619526 | -1.08253 |
| 5 | 25 | 15 | 6 | 46 | 0.23 | -0.378591 | -0.986591 |

<!-- toyquant:check
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=2 --refresh-price-ticks=4 --order-size=10 --inventory-limit=40
then: trade_reports=11 max_abs_inventory=21 taker_fees=0 gross_pnl_usd=0.315192 net_pnl_usd=0.107192
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=2 --refresh-price-ticks=4 --order-size=10 --inventory-limit=40 --flatten-band=10
then: trade_reports=21 max_abs_inventory=20 flatten_orders=3 flatten_quantity=21 taker_fees=0.105 gross_pnl_usd=-0.619526 net_pnl_usd=-1.08253
run: l2_replay data/v2/deribit_trades_2020-04-01_BTC-PERPETUAL.csv.gz /tmp/depth_200k_base.csv BTC-PERPETUAL 0 active_l2 1 prorata --fast-validation --base-spread-ticks=2 --refresh-price-ticks=4 --order-size=10 --inventory-limit=40 --flatten-band=5
then: trade_reports=25 max_abs_inventory=15 flatten_orders=6 flatten_quantity=46 taker_fees=0.23 gross_pnl_usd=-0.378591 net_pnl_usd=-0.986591
-->

The band does what it says — the peak position falls from 21 units to 15 as the band tightens — and it costs more
than it saves: 0.105 to 0.23 USD of taker fees plus the spread on every flattening order. And the one positive row
is the row that carried its position: 210 USD of exposure into a favourable final mark is where its +0.107 came
from, and removing the position removes the profit. **The position paid, not the quoting.** That is the same
statement the closed-versus-total split makes from the other side, and it is why the flatten band stays off by
default.

### What this leaves

Both mechanisms work, and neither makes the quoting profitable on this instrument: at every width where fills
happen at all, the capture does not cover the fee plus what the fills cost. That is the argument for the last step
rather than another attempt at this one — a market whose spread is wide relative to its fee, where this same
decision layer has something to capture in the first place.

## The Market Where the Fee Is Not the Wall

The last two sections end on the same sentence: on a perpetual the fee is 2.57 ticks against a capture of 0.095,
so nothing about the quoting can matter until the instrument changes. Two markets where the fee is a *fraction*
of a tick are reachable from a Hong Kong personal account, and both are now measured with the decision layer the
perpetuals were measured with, on the same two hours of one session (2026-09-29 13:30 to 15:30 UTC, the US cash
open), from Databento `mbp-10` downloads converted by `tools/fetch_databento.py` into the CSV pair the existing
readers take. A micro E-mini S&P 500 contract pays about 0.62 USD of commission against a tick worth 1.25, and a
US share pays 0.0035 USD against a tick of one cent.

| Window | Fills | Units | Fee USD | Capture, ticks | Markout, ticks | Gross USD | Net USD | Net per fill | Breakeven maker rate |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| micro E-mini, one tick | 1675 | 3934 | 2439.08 | 0.013091 | 0.157092 | -387.5 | -2826.58 | -1.68751 | -2.54567e-06 |
| micro E-mini, two ticks | 57 | 122 | 75.64 | 0.479508 | -0.688525 | -196.25 | -271.89 | -4.77 | -4.15733e-05 |
| micro E-mini, four ticks | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| micro E-mini, gate on | 1599 | 3750 | 2325 | 0.0110667 | 0.1596 | -98.75 | -2423.75 | -1.51579 | -6.806e-07 |
| **SPY, one tick** | 799 | 33352 | 116.732 | 0.267855 | 0.0919135 | 379.24 | 262.508 | 0.328546 | 1.48655e-05 |
| SPY, two ticks | 263 | 12279 | 42.9765 | 0.672978 | -0.246926 | -32.83 | -75.8065 | -0.288238 | -3.49544e-06 |
| SPY, one tick, gate on | 791 | 33116 | 115.906 | 0.247146 | 0.111895 | 216.18 | 100.274 | 0.126769 | 8.53436e-06 |
| SPY, one tick, flattening above 1000 shares | 832 | 34618 | 121.163 | 0.241161 | 0.114406 | 282.58 | 161.417 | 0.194011 | 1.07956e-05 |

<!-- toyquant:check
run: l2_replay data/v2/xnas_SPY_trades_2026-09-29_1330_1530.csv.gz data/v2/xnas_SPY_depth_2026-09-29_1330_1530.csv.gz SPY 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1
then: trade_reports=799 trade_report_quantity=33352 fees_paid=116.732 captured_edge_per_unit_ticks=0.267855 markout_per_unit_ticks=0.0919135 gross_pnl_usd=379.24 closed_pnl_usd=330.519 net_pnl_usd=262.508 net_per_fill_usd=0.328546 breakeven_maker_rate=1.48655e-05 max_abs_inventory=1099
run: l2_replay data/v2/xnas_AAPL_trades_2026-09-29_1330_1530.csv.gz data/v2/xnas_AAPL_depth_2026-09-29_1330_1530.csv.gz AAPL 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1
then: trade_reports=2324 trade_report_quantity=60167 fees_paid=210.584 captured_edge_per_unit_ticks=0.640152 markout_per_unit_ticks=0.042515 gross_pnl_usd=-2954.95 closed_pnl_usd=-3103.2 net_pnl_usd=-3165.53 net_per_fill_usd=-1.36211 breakeven_maker_rate=-0.000147613 max_abs_inventory=1096
run: l2_replay data/v2/xnas_TSLA_trades_2026-09-29_1330_1530.csv.gz data/v2/xnas_TSLA_depth_2026-09-29_1330_1530.csv.gz TSLA 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1
then: trade_reports=1894 trade_report_quantity=50753 fees_paid=177.635 captured_edge_per_unit_ticks=0.97942 markout_per_unit_ticks=0.34062 gross_pnl_usd=-1297.89 closed_pnl_usd=-1476.02 net_pnl_usd=-1475.53 net_per_fill_usd=-0.779053 breakeven_maker_rate=-7.22386e-05 max_abs_inventory=1098
-->

The whole table is re-derived by `tools/f4_windows.sh`, which needs the four converted file pairs and says so
rather than printing numbers without them. The pinned rows are the three claims the per-edit gate keeps true, one
per market: SPY, which put the fee question in play, and AAPL and TSLA, whose capture is the first to cover the fee
at every queue model. Each costs about three seconds, which is affordable inside the loop that runs on every edit;
the other nineteen rows cost about a minute together, which is not, so the script is their re-derivation instead.

Three of the rows are the same finding the perpetuals produced, so they are read the same way. Widening the quote
multiplies the capture and cuts the fills — on SPY from 799 fills to 263, and on the micro E-mini from 1,675 to 57
before four ticks produce none at all in two hours. Flattening the position costs more than it saves, on SPY
lowering 262.508 to 161.417, which is now the second market to say so. And the 0.013091 of capture on the
micro E-mini is a hundredth of a tick against a fee of half a tick: a fee a tick can cover is not the same thing
as a capture that a fee can pay for, and the micro E-mini is where those two came apart.

Two rows are new, and they are the reason this section exists. **SPY's closed PnL is +447.251 USD gross and
+330.519 after fees**, so the 262.508 net is quoting rather than a position carried into a favourable mark — the
opposite of the perpetuals row the flatten-band section ends on. And its **breakeven maker rate is +0.0000149
rather than negative**: a rate on top of the commission would still leave the window flat, where every earlier
window needed a rebate. The other new row is the gate behaving as a filter rather than an improvement: it cuts
the micro E-mini gross loss from -387.5 to -98.75, which is its purpose, and on SPY, whose measured markout is
positive, it lowers +262.508 to +100.274 by refusing fills that were paying.

What this is not: the book is Nasdaq's alone, so the queue ahead of a quote is one venue's displayed size out of
a consolidated market and the fill population is optimistic where it matters; it is two hours of one session
taken from the part of the day with the most mean reversion; the commission is a large retail broker's tiered
rate and the tier a Hong Kong account receives is unverified; and the venue's own maker rebate, of the same order
as that commission, is not in the model. The claim this section makes is only that the inequality the perpetuals
could never satisfy **is satisfiable**, and that the row which satisfies it says so in a field the gate
re-derives.

### The Latency That Window Would Have Needed

The row above was measured with a synchronous engine, which is the one assumption a personal account cannot
make: the strategy decides and the exchange has the order in the same instant. `tools/latency_sweep.sh` varies
the two latencies the engine models, order and cancel, on the same window and the same policy, and then on a
slower policy as a control, because a quote that lives longer ought to be less exposed to delay if delay is the
problem.

| Window | order/cancel | fills | shares | cancels | capture ticks | gross USD | net USD | net/fill USD | breakeven rate |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| SPY fast, synchronous | 0/0 | 799 | 33352 | 51903 | 0.267855 | 379.24 | 262.508 | 0.328546 | 1.48655e-05 |
| SPY fast, 1 ms each way | 1000/1000 | 921 | 38487 | 52262 | 0.213825 | 136.59 | 1.8855 | 0.00204723 | 4.6397e-06 |
| SPY fast, 5 ms each way | 5000/5000 | 9 | 500 | 113 | -0.08 | -1142 | -1143.75 | -127.083 | -0.00297873 |
| SPY fast, 20 ms each way | 20000/20000 | 13 | 600 | 77 | -0.166667 | -753 | -755.1 | -58.0846 | -0.00163672 |
| SPY fast, 50 ms each way | 50000/50000 | 23 | 918 | 51 | -0.533769 | -1040.86 | -1044.07 | -45.3945 | -0.00147879 |
| SPY fast, 20 ms order only | 20000/0 | 12 | 500 | 78 | -0.6 | -371 | -372.75 | -31.0625 | -0.000967678 |
| SPY fast, 20 ms cancel only | 0/20000 | 928 | 39588 | 51632 | 0.213082 | 232.92 | 94.362 | 0.101683 | 7.69175e-06 |
| SPY slow refresh 8, synchronous | 0/0 | 1720 | 66409 | 16599 | -0.354929 | 5.22 | -227.212 | -0.1321 | 1.02766e-07 |
| SPY slow refresh 8, 5 ms each way | 5000/5000 | 1510 | 58240 | 12899 | -0.445167 | -567.25 | -771.09 | -0.510656 | -1.27345e-05 |
| SPY slow refresh 16, synchronous | 0/0 | 1710 | 65571 | 16630 | -0.40553 | 106.01 | -123.489 | -0.0722155 | 2.11372e-06 |
| SPY slow refresh 8, two ticks wide, 5 ms each way | 5000/5000 | 890 | 35043 | 13986 | -0.253874 | 333.69 | 211.04 | 0.237123 | 1.24485e-05 |
| MES fast, synchronous | 0/0 | 1675 | 3934 | 23823 | 0.013091 | -387.5 | -2826.58 | -1.68751 | -2.54567e-06 |
| MES fast, 5 ms each way | 5000/5000 | 1671 | 3941 | 23639 | 0.0185232 | -610.625 | -3054.04 | -1.82768 | -4.00432e-06 |

<!-- toyquant:check
run: l2_replay data/v2/xnas_SPY_trades_2026-09-29_1330_1530.csv.gz data/v2/xnas_SPY_depth_2026-09-29_1330_1530.csv.gz SPY 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1 --order-latency-us=1000 --cancel-latency-us=1000
then: trade_reports=921 trade_report_quantity=38487 cancel_requests=52262 captured_edge_per_unit_ticks=0.213825 gross_pnl_usd=136.59 net_pnl_usd=1.8855 net_per_fill_usd=0.00204723 breakeven_maker_rate=4.6397e-06
run: l2_replay data/v2/xnas_SPY_trades_2026-09-29_1330_1530.csv.gz data/v2/xnas_SPY_depth_2026-09-29_1330_1530.csv.gz SPY 0 active_l2 1 conservative --order-size=100 --inventory-limit=1000 --base-spread-ticks=1 --order-latency-us=5000 --cancel-latency-us=5000
then: trade_reports=9 trade_report_quantity=500 cancel_requests=113 captured_edge_per_unit_ticks=-0.08 gross_pnl_usd=-1142 net_pnl_usd=-1143.75 net_per_fill_usd=-127.083 breakeven_maker_rate=-0.00297873
-->

Re-deriving all thirteen rows costs about a minute and a half, so only the crossing and the collapse are pinned
and the script prints the rest.

**One millisecond each way is where the positive row goes to zero**, and the mechanism is not that latency stops
you trading: fills *rise*, from 799 to 921, while the capture per fill falls from 0.267855 to 0.213825 ticks, so
the delay changes which fills you get rather than how many. The margin the window had was 0.328546 USD a fill,
and the delay cost is of the same order, which is what a margin of a third of a cent a share means against a one
cent tick.

**Five milliseconds gives the price away.** Fills collapse from 799 to 9, because a quote that arrives late
arrives while the market is already somewhere else, and the run ends holding 500 shares marked at -1142, which is
where its -1143.75 comes from: the position again rather than the quoting.

**The two latencies are not symmetric, and order latency is the damaging one.** Twenty milliseconds on orders
alone is as bad as both at once, at 12 fills and -372.75, because an order that becomes live late becomes live at
the wrong price. Twenty milliseconds on cancels alone is far milder, at 928 fills and +94.36: stale quotes get
filled more often and each of them slightly worse, which is a cost rather than a cliff.

**Slowing the quotes down does not rescue it, and this is the result worth keeping.** A policy that re-quotes
only after eight or sixteen ticks of fair-price movement, at zero latency, earns -227.212 and -123.489 with a
capture of -0.354929 and -0.40553 ticks: the frequent cancelling was itself doing the fill selection, and a quote
that waits longer is a quote that gets adversely filled. The reflex answer to latency, which is to trade more
slowly, is measured here and it is wrong.

**One cell of the table is positive and it is not evidence.** At two ticks wide, with a slow refresh, at five
milliseconds each way, the run makes +211.04 — and its capture is still negative at -0.253874 with a closing
position of -237 shares, so the gross came from the position carried into a favourable mark, which is the shape
of the row the flatten-band section above ends on. Picking that cell after seeing the others is exactly the
window selection this repository refuses to do.

**Verdict: the sixth gate fails.** The tolerance of this policy on this window is about one millisecond one way,
which is not a personal account's to control, and the feed latency a real deployment adds on top of it is not
modelled at all, so a live result would be worse than the worst row above. What this does not yet close is the
one cheap test the arithmetic leaves open: a symbol whose spread is three or more ticks wide has a larger capture
per fill to absorb the same delay. That is the screen in gate 1, and it is the last thing to try before the
quoting line is written down as a research result rather than as a plan.

## The Screen, Run: the First Two Symbols That Pass It

That screen has now been run, and this is the result. The tool is `tools/screen_spread.py`, the rule is the one
pre-registered in the [Roadmap](ROADMAP.md) -- the half spread where trades print, at least three times the 0.35
ticks a share pays per side -- the activity floor of 2.8 trade prints a second was written down before the run
rather than after it, and the cost was **0.52 USD** of the historical credit for thirty symbols of top of book over
the same two hours:

| symbol | median book spread, ticks | median spread where trades print | half at trades / fee per side, needs 3 | trade prints a second, floor 2.8 | spread | activity |
|---|---:|---:|---:|---:|---|---|
| SPY | 2.000 | 1.000 | 1.43 | 3.97 | fail | pass |
| QQQ | 3.000 | 2.000 | 2.86 | 3.64 | fail | pass |
| **AAPL** | 4.000 | 3.000 | **4.29** | 7.65 | **pass** | **pass** |
| **TSLA** | 6.000 | 5.000 | **7.14** | 6.86 | **pass** | **pass** |
| JNUG | 41.000 | 30.500 | 43.57 | 0.016 | pass | fail |
| NUGT | 24.000 | 19.000 | 27.14 | 0.042 | pass | fail |
| LABU | 105.000 | 68.000 | 97.14 | 0.054 | pass | fail |
| EWW | 4.000 | 2.000 | 2.86 | 0.063 | fail | fail |
| ARKK | 3.000 | 2.000 | 2.86 | 0.402 | fail | fail |
| USO | 3.000 | 2.000 | 2.86 | 0.504 | fail | fail |
| the other twenty, among them GME, AMC, SOFI, BOIL, UNG and the rest of the leveraged funds | 1.000 | 1.000 | 1.43 | under 1.4 | fail | fail |

The lines that produce these are `tools/screen_spread.py --cost --symbols ...` before any spend and then the same
run with `--download --max-cost-usd=... --min-trades-per-second=2.8`, which prints one `[SCREEN] symbol=...` line
per name with every field above in it; the downloaded files are cached under `data/v2/screen/`, so
`--input data/v2/screen/AAPL_2026-09-29T1330_2026-09-29T1530.csv` re-derives a row without buying anything again.

Three things come out of the run, and the third one is the reason this section exists.

**The screen agrees with the two windows already recorded.** SPY comes back at a 2.000 median book spread and a
1.000 spread where trades print, which is the pair the F4 section above measured from ten levels of depth, and its
**3.97 trade prints a second is the number the activity floor's own table was derived from**. A screen that
disagreed with those two would have been the thing to question, and it does not.

**The floor is what removes the widest spreads, and that is the case it was written for.** JNUG's spread is 43.57
times the fee per side and its quotes are the ones a quoter would want; it trades 0.016 times a second, which is
one print every sixty seconds, so a resting quote there is not placed in a market. NUGT and LABU are the same
shape at 0.042 and 0.054. A screen that ranked on spread alone would have put those three first.

**The symbols that pass are among the most liquid in the market, not away from it.** SPY fails at 1.43 and the
perpetuals the project is built on fail by two orders of magnitude, so the roadmap said the search had to leave
the most liquid symbol in the world. It does not: **AAPL and TSLA are the ones that pass**, at 4.29 and 7.14 times
the fee per side, and the mechanism is a property of a single venue's book rather than of the symbol. A probe over
the same files, which reproduces SPY's 2.000 and 1.000 exactly, says where the width is:

| | AAPL, mean 332.71 | TSLA, mean 354.00 | SPY, mean 764.96 |
|---|---:|---:|---:|
| share of book rows at a 1 tick spread | 6.2% | 2.2% | 18.7% |
| book spread, median | 4 ticks | 6 ticks | 2 ticks |
| spread where trades print, median | 3 ticks | 5 ticks | 1 tick |
| trades that print on the venue's own touch | 71.9% | 72.5% | 85.4% |
| trades that print inside the venue's spread | 28.1% | 27.6% | 14.6% |

So the wide spread is not an artefact of measuring one venue's book: two thirds to three quarters of the trades
that print on that venue print against the touch of that same wide book. What the table does not say is what
happens to a quote that rests there, which is the next measurement, and it is the one the fill model exists for.

### What the two symbols do in the engine

Same policy, same two hours, ten levels of depth, one row per queue model, and the fee column is the same 0.35
ticks a fill this repository has charged since the F4 windows:

| window | fills | units | fee USD | capture, ticks | capture / fee per side | markout, ticks | net USD | peak inventory |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| SPY, one tick, for reference | 799 | 33,352 | 116.73 | 0.268 | 0.77 | +0.092 | +262.51 | 1,099 |
| AAPL, `conservative` | 2,324 | 60,167 | 210.58 | 0.640 | **1.83** | +0.043 | -3,165.53 | 1,096 |
| AAPL, `prorata` | 4,673 | 110,143 | 385.50 | 1.060 | 3.03 | -0.297 | -2,229.35 | 1,099 |
| AAPL, `lumpy` | 4,412 | 109,002 | 381.51 | 1.045 | 2.99 | -0.293 | -2,143.97 | 1,086 |
| AAPL, `optimistic` | 4,919 | 123,319 | 431.62 | 1.102 | 3.15 | -0.347 | -2,403.80 | 1,096 |
| TSLA, `conservative` | 1,894 | 50,753 | 177.64 | 0.979 | **2.80** | +0.341 | -1,475.53 | 1,098 |
| TSLA, `prorata` | 3,918 | 100,908 | 353.18 | 1.623 | 4.64 | -0.112 | -2,311.61 | 1,098 |
| TSLA, `lumpy` | 3,731 | 99,158 | 347.05 | 1.611 | 4.60 | -0.112 | -2,221.04 | 1,082 |
| TSLA, `optimistic` | 4,082 | 107,260 | 375.41 | 1.682 | 4.80 | -0.166 | -2,343.31 | 1,083 |

**This is the first market in the repository where the quoting edge covers the commission, and it covers it at
every point of the model space.** Where SPY's conservative floor covers 0.77 of the fee, AAPL's covers 1.83 and
TSLA's 2.80, and neither is a margin the model can erase: the floor is the lower bound of the four, and it is above
one. The screen predicted the direction of this -- a half spread of 1.5 and 2.5 ticks at the trades -- and the
engine's realised capture is 0.64 and 0.98, which is the halving the tool's own closing paragraph warns about and
the same factor (0.54) the F4 section recorded between SPY's half spread and what a fill captured.

**And both windows still lose money, for a reason the fee table cannot see.** At the conservative model AAPL's
capture is worth **+385.16 USD** against 210.58 of commission, so the quotes paid for themselves and 174.58 USD
over; the window's gross is **-2,954.95**, which leaves about **3,340 USD** of adverse price on the position the run
was carrying. TSLA is the same shape at a different size: capture +497.09 against 177.64 of commission, gross
-1,297.89, so about 1,795 USD of drift. The drift is not adverse selection -- AAPL's measured markout is *positive*
(+0.043 ticks), so the fills were not picked off -- it is the mark on 1,096 and 1,098 shares of inventory,
0.36 and 0.39 million USD of notional, over two hours in which the price moved against it by a few tenths of a
percent. At that size a 0.9% move is 3,300 USD and the whole quoting edge is 385.

**The exposure is what moves the answer, and the run controls it.** Tightening the inventory limit lowers the loss
monotonically on both symbols, because the drift term is proportional to the exposure while the capture is
proportional to the traded volume:

| window, same policy | fills | units | capture, ticks | fee USD | net USD | peak inventory |
|---|---:|---:|---:|---:|---:|---:|
| AAPL, inventory limit 1,000 shares | 2,324 | 60,167 | 0.640 | 210.58 | -3,165.53 | 1,096 |
| AAPL, limit 1,000, flattening above 300 | 2,844 | 71,952 | 0.297 | 251.83 | -1,881.13 | 400 |
| AAPL, limit 200 shares | 2,240 | 57,813 | 0.642 | 202.35 | -1,454.75 | 299 |
| AAPL, limit derived from the instrument (one share, one 100 share order) | 1,837 | 46,356 | 0.398 | 162.25 | **-870.88** | 100 |
| TSLA, inventory limit 1,000 shares | 1,894 | 50,753 | 0.979 | 177.64 | -1,475.53 | 1,098 |
| TSLA, limit 1,000, flattening above 300 | 2,306 | 60,143 | 0.562 | 210.50 | -1,360.77 | 400 |
| TSLA, limit 200 shares | 1,783 | 47,402 | 0.973 | 165.91 | -1,246.20 | 297 |
| TSLA, limit derived from the instrument | 1,239 | 31,851 | 0.873 | 111.48 | **-844.22** | 100 |

Cutting the peak position from 1,096 shares to 100 takes AAPL's loss from 3,165.53 to 870.88, which is a quarter
of it, and TSLA's from 1,475.53 to 844.22. The flatten band does the same thing less cheaply at 1,000 shares of
limit, which is the second row of each block: it realises the position at the touch instead of carrying it, so it
pays the spread and the fee on every flatten, and AAPL's capture falls from 0.640 to 0.297 because of it. The
distinction matters for reading the SPY rows above, where flattening *lowered* the result from 262.51 to 161.42:
there the drift was in the position's favour and here it is against it, so the same control has opposite signs on
the two. What is stable across both is the size of the term, not its sign.

The derived limit is worth one sentence because it is the harness's own default: `strategy_factory.cpp` sizes it as
the instrument's quantity scale divided by ten, which for a US share -- one unit per share -- is **one share**, so a
run that passes no limit holds at most one unfilled order of 100 shares and the peak inventory is 100. Every row in
the F4 table above was measured with an explicit limit of 1,000 shares, which is the setting that made the exposure
term large enough to dominate; the same measurement at the harness's default would have shown a smaller number for
SPY as well, and a different sign on it.

### What this changes

The wall moved rather than fell. On these two symbols the fee is no longer the binding constraint -- the capture
clears it at the lower bound of the queue model, which is exactly what the screen was for -- and what is left is a
**position sizing and drift** question of the same shape the perpetuals had from the other side.

**The next measurement is across days, and it now has a specific question.** The capture, the fee and the fill
count are stable across the model space on both symbols; what is not stable is the sign of the drift, and one
two-hour window cannot separate a strategy from a lucky direction. That is gate 2, and the budget section's
sixty-odd sessions are what answer it: capture per fill and realised net on every session, with the closing position
marked and the exposure held to the smallest limit that still produces fills.

**None of this says the strategy pays, and the numbers that do not are large.** Both symbols lose money in the
session measured, and at the 1,000 share limit the losses are 8.2 times AAPL's quoting edge and 3.0 times TSLA's;
the one number that would say otherwise -- a positive net at a controlled exposure across several sessions with
different price directions -- does not exist yet. The honest summary of this section is one sentence: **the fee
stopped being the reason these two symbols fail, and something else has to be measured before they stop failing.**

The caveats the F4 section records apply to every row above, and there is one more. The book is **Nasdaq's alone**,
so the queue ahead of a quote is one venue's displayed size out of a consolidated market and the fill population is
optimistic where it matters; it is **two hours of one session**, taken from the part of the day with the most mean
reversion; the commission is a large retail broker's tiered rate and **the tier a Hong Kong account receives is
unverified**; and the venue's own maker rebate, of the same order as the commission, is not in the model. The extra
one is the harness's: the runs start with **1,000 USD of cash and carry up to 0.39 million USD of notional**, so the
PnL is measured on exposure the account is not funding, and `exposure_over_collateral_fills` counts every fill that
made that worse. A real account would hold that position against more capital or a smaller position against that
capital, and the row whose peak inventory is 100 shares is what the second of those looks like.

### What the account's fee schedule adds

Reading the broker's own pages on 2026-10-02 for what those terms actually are, they are these:

| term | how it is charged | AAPL, 332.71 a share | SPY, 764.96 a share |
|---|---:|---:|---:|
| the share commission, Tiered at this account's volume | per share, both sides | +0.35 ticks | +0.35 ticks |
| the Nasdaq credit for adding displayed liquidity, from the broker's own NASDAQ/Island pass-through table (read there: remove 0.0030 USD, add (0.0013) USD) | per share, on a passive fill | **-0.13 ticks** | not read |
| the SEC fee, charged on **proceeds**, at the published 0.0000206 | per share sold | **+0.685 ticks** | **+1.58 ticks** |
| the NSCC and DTC clearing fee, 0.0002 USD a share | per share | +0.02 ticks | +0.02 ticks |
| the FINRA consolidated audit trail fee, 0.000003 USD a share | per share | +0.0003 ticks | +0.0003 ticks |
| the FINRA TAF | per share sold | prints as 0.00 on the schedule | 0.00 |

A run's fills are about half buys and half sells, so the arrangements the account can actually choose cost this per
fill, against the **0.640 ticks** the AAPL conservative row captured:

| arrangement | per fill | against the capture |
|---|---:|---:|
| Tiered, SmartRouted, exchange credit passed through | **0.583 ticks** | **+0.057 ticks, about 34 USD over the window** |
| Tiered, SmartRouted, credit not passed through | 0.713 ticks | -0.073 ticks |
| Fixed, SmartRouted, exchange fee bundled into the rate | 0.863 ticks | -0.223 ticks |
| Fixed with API direct routing, 0.0075 USD a share | 1.113 ticks | -0.473 ticks |

**So the fee schedule decides nothing, and the arrangement that would let a quote choose its venue is the worst of
the four.** The best case leaves 0.057 of a tick a fill -- 34 USD on a two hour window whose position cost 3,340 --
and every other case is negative. That is why the venue question is answered by recording where orders land rather
than by paying for routing, and why the cross-day measurement is the only test left standing.

Read across symbols, the same table says something the screen rule does not: because the SEC fee is charged on
proceeds, **it grows with the share price** -- 1.58 ticks a side to sell a 765 dollar ETF against 0.685 on a 332
dollar share -- and at 0.685 it is **the largest single term in the row, 59% of the 0.583**, larger than the
commission the model has been pricing all along. The rule's "three times the fee per side" is therefore not price
independent, and the correction points the same way the rest of the equity evidence does -- at cheaper shares with
wider spreads.

**One term could still turn the sign, and the broker's own note about it is written for a strategy that requotes.**
Every market's commission table this repository could read carries a "minimum per order" row -- Japan, Singapore,
Malaysia and India all print one, in their own currencies -- the US Fixed examples on the same page demonstrate
1.00 USD for a 100 share order at 25 USD, and the API direct routed row prints `Minimum per order USD 1.00`; the
Tiered figure this repository has been given is **USD 0.35**, and it is recorded here as reported rather than read,
because three attempts to fetch the US commission table found it in the part of the page the fetcher truncates.
What was read verbatim, in the Chinese rendering of the same page, is the note that decides how such a minimum
applies:

> "modifying an order is treated as cancelling the original order and replacing it with a new one. At some
> exchanges, this can cause the modified order to be charged commission as if it were a new order. For example, if
> you submit a 200 share order, 100 shares execute, and then you modify the order and the remaining 100 shares also
> execute, then both 100 share orders are subject to the minimum commission."

The counter that decides it now exists, and on this window the answer is that the minimum applies. The run reports
the filled order **instances** and their sizes, and AAPL shows **1,222 instances against 2,324 fills**, with
**969 of them -- 79% -- filling fewer than 100 shares**, which is where 0.35 stops binding at a 0.0035 share rate:

| filled order size | orders | shares |
|---|---:|---:|
| up to 50 | 737 | 17,738 |
| 51 to 99 | 232 | 17,129 |
| 100 to 199 | 253 | 25,300 |

So a 0.35 minimum charged per order instance costs `969 * 0.35 + 0.0035 * 25,300` = **427.70 USD** against the
**210.58 USD** the per share model charges, a factor of 2.03, and the per fill cost moves from **0.583 to 0.943
ticks** against a capture of 0.640. **The quoting therefore stops covering its fee**, short by about 0.30 of a tick
a fill. The window's sign does not move: 0.30 of a tick over 60,167 shares is 183 USD on a window whose position
term is 3,340.

The same three buckets also say why the minimum binds and what would unbind it, and it is not a fee finding. The
strategy replaces **45,680 of its 45,907 orders** and fills 26 shares at a time, so nearly every order it fills
fills partially; the 253 orders that did fill 100 shares or more paid the per share rate instead. Getting the other
969 into that bucket means quoting larger, which is the same direction as the drift term that costs 3,340 and
against it, so the two have to be traded off rather than fixed separately. TSLA reads the same way -- 1,128
instances, 920 under 100 shares, 394.80 USD against 177.64, 1.011 ticks a fill against a capture of 0.979, a
shortfall of 0.03 of a tick -- and SPY reads 529 instances with 324 under 100 shares, though SPY's fee was never
covered to begin with.

Nothing in this section moves a measured number: every table above is what the engine printed, and the terms here
are prose beside them so that a reader can see what the engine does not carry. `fees_paid` is still the per share
figure on purpose -- counting what the broker charges and changing what the engine charges are two changes, and
this section needed the first. The second is now named and sized: two fees on a sale, a credit on a passive fill,
and a minimum per order instance, carried by the instrument rather than by prose. It is worth making, because it is
what turns every net figure in the equity rows into one a broker's statement would agree with, and it should be made
with the rates from a first trade confirmation rather than from a page -- the two readings of the minimum are 183
USD apart on this window, and a confirmation is what settles which one is real.



