# Changelog

## The live paper milestone (current)

The websocket reader, the paper mode and the recorder half of a live path, which is roadmap phase 2: the
seam it closes is the one the roadmap names, that a live feed is another reader and paper trading is the
existing book and matching engine against it. So the live path is the L2 replay path with its two readers
moved onto a socket, and everything downstream of them is the same code. Its entry point is the `ws` mode in
[User Guide](docs/USER_GUIDE.md), and the numbers the other documents quote are re-derived by
`bash tools/verify_l2.sh fast`, which this milestone leaves unchanged.

### Highlights

- **A websocket feed behind the reader interface.** `src/market/ws_feed.*` speaks RFC 6455 -- the handshake
  and text frames -- with no external dependency, and implements `IMarketEventReader` as two readers over one
  connection, so the book, the strategy, the engine, the recorder and the paper broker cannot tell a live
  session from a replayed one.
- **The wire is the replay format.** One CSV row per text message, its kind read from its column count, in the
  two shapes `--record` writes, so a recording streamed back over the feed reproduces itself byte for byte.
- **A desk loop with no credential.** `tools/websocket_sender.py` stands a WebSocket server in for a venue and
  streams a trade and a depth file, interleaved by their own timestamps, so the whole path runs locally. A
  unit test closes the same loop inside `ctest`, with a server thread standing in for the venue.
- **One mode, reused.** `ws` accepts every `l2_replay` flag -- the recorder, the feed thread, the profiler and
  the paper broker -- because it is that mode with the readers swapped, and `run_l2_replay_mode` is now
  `run_l2_session_mode`, which owns both sources.
- **A bounded session and a bounded handshake.** `--ws-seconds=N` ends a capture whose peer never closes the
  socket, draining what already arrived first, and the connect and the opening handshake share a five-second
  budget, so a host that accepts the connection and then says nothing fails the run rather than hanging it.
- **A relay that survives an idle socket and a slow reader.** Both heartbeat forms are answered -- a text
  `ping` and a control ping -- which a six second run never exercises; the hand-off to the engine is bounded,
  so a slow reader slows the venue down instead of growing the process, and the wait and its high-water mark
  are printed with the relay's summary. The feed reports its own `ring_waits`, and both recorders are flushed
  as they go, so a stopped session loses at most the last few thousand rows. Two test hooks make the two
  paths executable against a venue: `--ping-seconds=N` sends a ping on a fixed cadence even while data
  flows, and `--drop-after-seconds=N` closes and reopens the venue connection.
- **A thirty-minute live soak that replays into itself.** OKX's public `BTC-USDT-SWAP` channel, the paper
  broker and both recorders for 1,800 seconds: 697,400 frames, 6,719 trades, 690,679 depth updates, with no
  sequence gap, no reconnect, no parse error and no ring wait, flat RSS in both processes, and a replay of the
  engine's recording that reproduces the live summary field for field. The heartbeat path stayed idle, so it
  is covered by a local stand-in rather than claimed against the venue.
- **A live merge that cannot deadlock on a trade gap.** A two-hour run with a forced reconnect every 25
  minutes stopped at the second drop: the merge waits for the next trade, depth keeps arriving, and the
  4,096-row depth ring (10.7 s at the measured 384 rows/s) filled while the receive thread blocked on a push
  the merge could not drain. The live input rings are now 131,072 rows, about 5.7 minutes of depth, and the
  feed reports `ring_high_water`. The structural fix -- one ordered stream instead of two blocking readers --
  is recorded in the roadmap.
- **The two-hour rerun, with the whole live path exercised.** 5,704,944 frames, 144,755 trades, 5,560,187 depth
  updates, four forced reconnects with zero sequence gaps, 355 pings answered by 355 pongs, no ring wait, a
  ring high-water of 3,917 against the old 4,096 capacity, five snapshot batches (the initial one and one per
  reconnect), a reconciliation that stayed `ok`, and a replay of the engine's recording that reproduced the
  live summary field for field.
- **One ordered stream on the live path.** `WebSocketFeed` now has one input ring and one reader: the receive
  thread assembles a run of depth rows that share a local timestamp, holds a trade that arrives during the
  run until the batch ends, and pushes both in that order. A trade gap can no longer stall depth delivery,
  which is the deadlock the two-ring merge reached; the ring is sized at 131,072 rows and reports
  `ring_high_water`. `--ws-ring-capacity=N` sizes it and `--idle-wait-us=N` trades a little latency for a core.
- **The edges around it.** The relay leaves subscription acknowledgements out of a capture, resets its backoff
  after a session that actually lasted, and the C++ twin gains `--ping-seconds` and `--drop-after-seconds`; the
  recorder flushes every 4,096 events; and a quote scaled below the instrument's minimum is raised to it, so
  the engine stops refusing the strategy's own size.
- **The client's half of the order path.** `OrderGateway` (`src/lines/order_gateway.*`) is a second decorator
  over the engine, wrapped outside the broker: it keeps the table of orders from submission to answer, absorbs
  a retry under the same client id, counts an order the venue has not answered within a timeout of market
  time, and re-sends it under the same id where the venue's duplicate check makes the retry safe. Reported as
  `[GATEWAY]`; with every knob at zero the run reports the numbers of the same run without it, and with a
  timeout and a retry the run reports the numbers of the same run with neither.
- **A participation cap that is only a capacity cap.** `--participation-limit=X` caps one aggressive
  external-BBO fill at X of the displayed best size. The fill price stays the touch and the remainder follows
  the order's time in force, so the simulator does not acquire a price-impact model or a new calibration.
- **IOC and FOK as order semantics, not strategy hacks.** `TimeInForce` lives on `StrategyOrder` and
  `exchange::Order`; the matcher returns its unfilled quantity, and the engine cancels an immediate remainder
  with `CancelReason::Ioc`. A FOK pre-checks external plus internal capacity, including the participation cap
  and self-trade stopping, then fills completely or cancels with `CancelReason::Fok` without a print.
  `--time-in-force=gtc|ioc|fok` applies it to a run; `tests/matching_engine_test.cpp` covers the cap, IOC,
  FOK-unfilled, FOK-filled and the split external/internal FOK. GTC remains the default, so existing runs are
  byte-for-byte unchanged.
- **A three-command demo path.** The User Guide's "A short demo path" runs `verify_l2.sh fast`,
  `risk_check.sh` and a one-cell `trend_sweep.sh` in the order their prerequisites require, with the exact
  lines to expect. It adds no strategy or script: the negative trend PnL is presented as the measured result
  rather than a failure, and the gate, the risk invariants and the cost of a taker signal are all visible in
  one sitting.
- **A smaller matching core.** The buy and sell sides now share one side-generic matcher, and the real
  external-BBO fill and the FOK pre-check share one touch predicate and one participation cap. The pipeline
  also has one `submit_order` step instead of a ninety-line loop body. This is a behavior-preserving
  refactor: the buy/sell report order, queue counters and all pinned run outputs are unchanged, and the gate
  stays green.
- **A named report path and a smaller order path.** The `Pipeline` constructor now installs a callback that
  calls `handle_report()` instead of holding a hundred-line lambda, and quote-distance statistics moved to
  `record_quote_observation()`, so generic order submission no longer carries market-making metrics. The
  report fan-out still runs in the same order and every pinned output is unchanged.
- **Market events named at the boundary.** `Pipeline::process_event` now dispatches through four named
  `handle_event` overloads instead of an `if constexpr` chain inside one visitor. Trade and BBO handling stay
  byte-for-byte the same, while a future non-MM event or strategy path has a named place to land.
- **Fill accounting as its own step.** `handle_report()` now keeps the report order and delegates one of
  our fills to `apply_market_maker_fill()`, which owns position, reservation, exposure, edge and markout
  updates. A future non-MM path can replace that fill accounting without copying the ledger and strategy
  callbacks.
- **Fill state in one component.** `FillAccounting` now owns position, position-gate reservations, maker
  notional, exposure and the trade counters; `Pipeline` reads it through accessors and keeps only the
  config inputs that also feed the report and engine. This removes eight mutable fields from the
  coordinator and gives a non-MM path one boundary to replace.
- **One event clock and one settlement step.** The L1, L2-view and L2-trade paths now share
  `begin_event()` (funding plus the event clock) and `advance_mid()` (midpoint, quote cycle and the
  five-cycle markout drain), and queue consumption is reported through `record_queue_activity()`. A
  venue report and a submission refusal settle through the same `settle_order()` call, so the
  minimum-commission charge cannot be forgotten on one of them. The helper bodies are the code they
  replaced, in the same order; no reported field changes.
- **A strategy that is not a market maker.** `trend_l2` is the other workflow on the same pipeline: an EWMA
  cross sets a target position, clips at the touch fill it, and a stop, a time stop or the exit band closes
  it. A cooldown after a stop keeps the next view from reopening the same position. It shares the pipeline's
  pre-trade gates instead of reimplementing them; on the 200k-row Deribit slice it filled 55 taker clips on
  both sides and reconciled `ok`. `--trend-params` exposes the signal, and `tools/trend_sweep.sh` replays a
  grid of it and writes the fills, PnL, fees and risk pauses to a CSV.
- **A pre-trade risk pipeline, and the measurement that keeps it honest.** The kill switch, the exposure cap
  and the position gate moved out of `submit_strategy_actions` into `RiskPipeline`, an ordered list of five
  checks; the two new ones are a fat-finger band around the midpoint and a token bucket on submissions. A
  run that names none of them is byte-for-byte the run it was, and a permissive setting leaves `[EXECUTION]`
  identical. The first measurement refused 161 and 33,685 orders; the second number was a retry storm, because
  a refusal was retried on the next view. `on_order_rejected` plus an exponential backoff in `trend_l2` (60
  views, doubling to 3,840) cut those to 13 and 15. A venue or broker refusal at submission takes the same
  path, in addition to the cancelled report that removes the order from the open set. The rate bucket, the
  exposure cap and the position gate also let a reducing order through, so a position that is too large can be
  worked down instead of trapped. `tools/risk_check.sh` holds the four runs.
- **A timeout is a query, not a fake cancel.** `OrderQuery` lets the gateway ask the venue whether it still
  holds an order that has not answered; `[GATEWAY]` reports `unacknowledged`, `timeouts_queried`,
  `timeouts_working` and `timeouts_unknown`. The ledger keeps the order `Pending` until a real report, because
  `Working` and `Unknown` are not a fill or a cancel. `tests/order_gateway_test.cpp` covers a timeout with no
  retry: the query is `Unknown` before the order lands, `Working` after it rests, and no terminal report is
  invented.
- **A terminal order state, including rejection.** `ExecType` gains `Rejected` and `RejectReason` (venue,
  broker, timeout, risk), and the pipeline, ledger and strategies share one `is_terminal` predicate: a
  terminal report releases the quantity the position gate reserved, closes the audit entry and removes the
  order from the strategy's open set. `Pipeline::handle_rejected_order` now reports a venue or broker refusal
  as `Rejected` rather than `Cancelled`, so a strategy can tell a refusal from a cancel it asked for;
  `[EXECUTION]` appends `audited_rejected_orders` only when there is one. `Cancelled` also carries a
  `CancelReason` (`user`, `venue`, `ioc`, `fok`, `self_trade`, `timeout`), so `trend_l2` backs off on a venue or
  self-trade cancel but re-quotes immediately after its own. The ledger now has an explicit state machine
  (Pending -> Resting -> PartiallyFilled -> Filled/Cancelled/Rejected); an illegal transition and a report for
  an already-closed audit are counted rather than applied, and `[EXECUTION]` prints those counters only when
  non-zero. `tests/order_state_test.cpp` checks that the next order is accepted after a refused one, which is
  the pending-reservation leak the old path could hide.
- **The account's equity series, behind a flag.** `--metrics` samples the marked equity once per
  60-second bucket at the point every event path already advances the midpoint, and `[PERFORMANCE]` reports
  the drawdown, how long the curve took to win its peak back, the annualized volatility, and the Sharpe,
  Sortino and Calmar ratios, scaled by the 525,600 buckets a 24/7 year holds. `Portfolio::equity_at` is the
  read-only valuation `mark_to_market` now shares, so a sample cannot move the unrealized PnL the kill switch
  reads in the middle of an event. Off by default, so every recorded line is byte for byte what it was.
- **The distribution behind the PnL.** `--metrics` also books every closing fill as a round trip: the side
  the position was opened on, its entry and exit price and time, the closing PnL, and what the trip cost —
  the exit fill's fee plus the share of the entry fees the position was carrying. `[ROUND_TRIPS]` reports the
  win rate, profit factor, expectancy, the average win and loss, the average holding time, and the aggressive
  and total notional against the starting collateral as turnover. A fill that closes part of a position and
  opens the rest splits its fee by quantity, so the sum of the trips plus the carried entry fees of the
  position still open is the run's realized PnL rather than a second opinion about it.
- **The distribution split by side.** `--metrics` also prints one `[ROUND_TRIPS_SIDE]` line per side that
  completed a round trip, with the same win rate, profit factor, expectancy, average win and loss and holding
  time. The aggregate hides which side a win came from: on the 2M slice one win in twelve is five longs with
  that single win at -0.022 USD a trade, and seven shorts with none at -0.010. The account-level notional and
  turnover stay on the aggregate line, because a fill belongs to the account rather than to the side of the
  position it closed.
- **An execution line with no view on direction.** `execution_l2` works one parent order from
  `--parent=SIDE:QUANTITY:SECONDS` with `--algo=market|twap:N|pov:X`, and the pipeline measures the parent
  itself rather than reading the strategy back: `[EXECUTION_ALGO]` reports what filled and the three terms of
  implementation shortfall in basis points of the arrival notional. The first measurement is a negative result
  about the *schedule*: on two non-adjacent windows no schedule beat a single crossing by 2 bps, and `twap`'s
  signed slippage flipped from +11.7 to -9.8 bps while the fee stayed at 4.6 to 5.0. The fee is the only term
  with a stable sign, which is what the maker-entry question would move and the schedule does not.
  `[EXECUTION_ALGO]` also reports the window's time-weighted midpoint, and against that benchmark `twap` lands
  within three basis points on both days while the single crossing's distance from it is the drift itself --
  which is what turns "the two windows disagree" into "the single crossing is a ten basis point bet, and the
  schedule pays a stable half point".
- **Posting instead of crossing, measured and rejected.** `--algo-entry=post:N` rests each slice at the near
  touch and crosses what it has not filled after `N` seconds. On two windows and four timeouts the fee does fall
  with patience (5.00 → 3.40 → 1.80 bps) but the units acquired fall with it (100 % → 60 %) and the opportunity
  cost rises faster (0.00 → 10.85 bps against the benchmark), so no timeout beats crossing. The taker fee is a
  floor on this venue rather than a parameter, and the remaining lever is a venue whose maker and taker rates
  differ by less than the fill risk costs.
- **Capacity, measured and flat.** A ladder from 1,000 to 100,000 contracts on both windows: the shortfall in
  basis points does not move at all, because every term is proportional to quantity, and what size changes is
  how many crossings the order needs (one on the deep day, six on the thin one, thirty-four to ninety-seven for
  the time-sliced schedule). No size flips the ranking, so "a parent larger than the display" stops being an
  open question rather than becoming a favourable one.
- **What the crossing side cost.** `--metrics` records the midpoint each order decided at and reports
  `[TAKER]`: the arrival slippage of every aggressive fill in ticks, basis points and dollars, the drift from
  the fill to the midpoint 5, 20 and 60 quote cycles later — positive when the move covered the spread, the
  opposite sign of the passive `adverse_selection` — and the implementation shortfall of the IOC/FOK orders
  split into the filled part, the fees, and the opportunity cost of the quantity they never got. A passive
  quote we cancel is deliberately not one of those orders: cancelling declines a fill rather than missing
  one. On the 200k-row slice with `trend_l2` under IOC, 55 crossing fills paid 1.53 ticks of arrival slippage
  and the drift never recovered it: -1.41, -1.55 and -1.44 ticks at 5, 20 and 60 cycles. `--markout-horizon=N`
  makes the cycle count behind the passive markout and the opportunity term a parameter; the default stays 5,
  so every recorded run reads the same.
- **The tick curve, drawn in five points instead of inferred from three.** The crossing floor is the fee plus half
  a tick, and the futures venue quotes every symbol on one 4 bps schedule while spacing its ticks by a factor of ten
  thousand, so it is where the curve can be measured rather than argued: `BTCUSDT` 0.014 bps of tick, `DOGEUSDT`
  0.52, `XRPUSDT` 1.63, against the two perpetuals at 0.78 and 3.76. Four of the five land on half a tick to three
  decimals. The fifth is the finding: `DOGEUSDT` pays 1.35 bps, five ticks, because its best ask holds thirty-six
  DOGE -- seven dollars -- so the parent walked the ladder in forty-five children, and shrinking it to seventy
  dollars still cost 0.73. The formula is `fee + max(half a tick, impact)`, and the depth term is the whole cost on
  a book whose touch is smaller than the venue's own minimum notional would suggest. That also re-reads the flat
  capacity ladder: it was flat because that book is deep, not because size never matters.
- **The maker hint, settled on a venue whose tick is negligible -- and an earlier claim of mine narrowed.** The
  BTC/ETH gap of 6.9 bps could not be attributed with 1 to 86 fills per window. The venue whose order book archive is
  free and daily from March 2023 offers 115 to 246 fills per window instead, and it also removes the variable under
  test: its BTC and ETH ticks are 0.015 and 0.033 bps of price, both a rounding error beside the 2 bps maker fee.
  There the two symbols are indistinguishable -- a gap of +0.10 bps across eight windows, with the two daily means on
  opposite sides of zero -- so the difference **cannot be the asset**: the venue that removed the tick removed the
  gap. What I first read as a measured gap on the coarse-tick venue (+7.44 bps over two days) does not survive four
  days: the daily gaps are -3.27, +8.14, -0.27 and +6.74, a standard error of 2.7 bps, so the sign and size there is
  what a few hundred fills cannot pin. The line stays closed either way: the fine-tick venue's gross is negative in
  all eight windows (-0.50 to -1.87 bps, before the fee) and the coarse-tick venue's ETH contract is negative on all
  four of its days. Frequency turned out not to be a knob at all: the same strategy holds a round trip 0.03 to 0.14
  seconds where the tick is 0.015 bps and 60 to 720 seconds where it is 0.78.
- **The dated basis, which the funding differential was mistaken for.** The twelve-month measurement that closed the
  carry line compared two venues' *perpetual funding*, where the differential flips sign inside a day. A dated future
  is a different animal: it carries a fixed gap to spot that must close at delivery. Six monthly samples of Deribit's
  free first days price 19 dated legs and 22 calendar spreads. The dated legs average **+252 bps annualised, 17 of 19
  positive**, and fail the registered gates on one count -- the near contract inverted twice, at -186 bps annualised
  in June as BTC fell from 77,889 to 72,043, which is the crash regime the BIS carry paper warns about. The calendar
  spreads, which need no spot at all, are **positive in 22 of 22 samples with a mean of +399 and a worst of +67 bps
  annualised** -- and that is recorded as a hypothesis rather than a result, because the gates were registered on the
  dated legs and swapping instruments after seeing both is how a sample gets picked to fit. The open limits are the
  ones a VWAP cannot answer: the dated books trade a hundredth of the perpetual's activity, so the spread paid to
  enter is unmeasured, and the six samples are one bull regime whose premium compressed 10x in July.
- **The revival test: can better execution rescue a negative edge? No.** The closing claim that execution cost is the
  one lever that does not decay was tested by asking whether any *quoting policy* on the fine-tick venue earns before
  the fee: nine combinations of width (2, 4, 8 ticks) and patience (refresh every 1, 4, 16 price moves), both symbols,
  same 200k windows. **Every cell is negative.** BTC improves monotonically with width and patience, from -1.39 to
  **-0.21 bps**, and is still nineteen basis points short of the 2 bps fee; ETH does not improve at all, sitting at
  -1.0 to -1.2 bps in all nine. So the difference execution can make -- 10 to 20 bps a round trip -- multiplies an edge
  and does not create one, and the loss here is in the *fill*, not in the plumbing: it is adverse selection, and
  execution does not select.
- **Selling options: measured three times, and only the third one counts.** The first pass priced the straddle from an
  at-the-money approximation on strikes up to 6.4 % away from spot and called the variance premium negative on ten
  samples. The audit split those by moneyness and read +19.9 points on the four that were genuinely at the money --
  which is the same error one level down. The measurement that counts chose the strike as the nearest 100, inverted
  Black-Scholes per leg, took the nearest listed expiry at least twenty days out, and covered **92 straddles from
  2020-01 to 2026-09**: implied **58.4 %** against realised **58.1 %**, a premium of **+0.2 volatility points**, and a
  short straddle that is a coin flip -- **51 of 92 positive, mean -105 USD, median +319, left tail -25,450 against a
  best of +8,367**. Implied exceeds realised in 63 % of months and the seller is still not paid on average, which is
  what a risk premium priced at fair value looks like. Two years pay, five do not.

  and `fees_bps_of_turnover`: the fees booked to the closed round trips, their mean, and those fees over the notional
  the run traded. `expectancy_usd` is already net of them, so the comparison that decides whether a run can pay at all
  is now two adjacent fields -- fees per round trip against gross edge -- instead of a subtraction a reader had to do
  from `[PNL]`. On the 200k slice a single round trip reports 0.004 USD of fees against 30 USD traded, 1.33 bps, which
  is the maker rate the run paid. The ratio divides by the run's own notional and not by `quantity * price`: the latter
  is not a notional on a fixed-face-value contract, and the first version of this field made that mistake and printed
  0.003 bps where the answer was 1.33.
- **The execution line reports which fee rate it paid, not just how much.** `[EXECUTION_ALGO]` gains `maker_share`,
  `maker_fees_bps` and `taker_fees_bps`: the share of the parent's filled quantity that rested, and the fee charged
  on each role, whose sum is the existing `is_fees_bps`. Every venue here prices the two roles differently -- 1.5
  against 3.5 bps on a dated future, 2 against 5 on a swap -- and the whole difference between the post-first
  experiment's fee of 1.80 bps and a crossing's 5.00 came from this mix, so a fee without it is not interpretable. A
  role the venue does not report is charged to neither side and stays visible as the gap against `is_fees_bps`. The
  invariant is asserted in `pipeline_accounting_test`: the two halves add up to the total on a parent worked half by
  resting and half by crossing.
- **Lead-lag, the last shape that would pay for speed, priced and closed.** BTC prints first and the alts follow within
  a few hundred milliseconds -- the one structure where a fast client is paid for reading one symbol and trading
  another. One free day of Binance's USDT-perp tape, 2024-03-28, 5,664 BTC triggers at the top percentile of size, five
  followers: **every follower is within 0.06 bps of the control, and the control itself is 0.03 to 0.11**. A round trip
  costs nine basis points, so the largest number in the table is a hundred and fiftieth of what it would need to be.
  That closes the fifteenth structure, and with it the shape of the whole answer: what is paid per action does not clear
  the fee charged per action, and what is paid per unit of time does not clear the noise it arrives through. Two things
  survive and neither is a strategy -- execution cost (CME 1.2 bps a round trip against 4 to 24 in crypto, and resting
  instead of crossing saves 10 to 20 of it) and breadth (two hundred half-basis-point trades a day across twenty symbols
  is a question about infrastructure, not alpha).
- **Three candidates that would need speed, priced before any code was written.** Each is a place where being fast is
  worth money for a measurable reason, and each was measured from free data. **The CME calendar**, held long the near
  and short the far over 120 sessions, drifts **+0.54 bps a day (+64.6 in total, +11.4 a month) against a round trip of
  1.94 bps** -- the cost is trivial and the drift is real, but the daily standard deviation is **29.76 bps, 55 times the
  drift**, so a holder is paid half a basis point to sit through thirty. **Large aggressive prints**, the footprint of a
  forced seller, do show continuation -- **+0.71 bps a second after a big buy, -0.68 after a big sell, against -0.09 for
  a random trade** -- and it is **eight times too small** for a round trip that costs 8. **A stale quote on one venue**,
  measured on 40.2 million aligned quote states on 2024-03-01, shows an executable gap between Binance's linear perp and
  Deribit's inverse perp that is **positive 99 % of the day at a median of +2.99 bps** -- a level, not an opportunity,
  which is why the harvestable part is the deviation from that level and not the level itself. The deviation, measured
  against each minute's median over 3.1 million merged states: **median 0.00, p99 +2.39 bps, and above the 9 bps a
  crossing round trip costs for 0.012 % of the day -- 16 spells, median 63 ms, under a minute in total.**
  **Together the three say the same thing**: the CME slope drifts 11.4 bps a month against a 1.94 bps round trip but
  carries 30 bps of daily noise; a forced seller continues 0.7 bps against an 8 bps round trip; a stale quote deviates
  2.4 bps at the 99th percentile against 9. Speed decides which side of those numbers you are on, not what they are.
- **The calendar basis passed its gates as a level and fails as a return -- my error, and the correction.** Ninety-one
  monthly samples from 2019 to 2026 price the calendar spread at a mean **+696 bps annualised, 85 of 91 positive**, and
  against a measured round trip of 20.5 to 30.2 bps it clears the three registered gates seven times over. That was the
  first passing line in this repository and it was wrong: **a level is not a return**. Holding each pair one month and
  pricing the same two contracts a month later gives 60 realised observations -- **mean -6.7 bps a month gross, 28 of
  60 positive, worst -330, best +386, and -30.7 bps after one round trip**. The arithmetic is simple once written:
  what converges to spot at its expiry is a future against spot; the gap between two futures converges to *the gap at
  the near expiry*, which is not zero. A calendar spread bets on the **slope** of the curve, and the slope moved up to
  330 bps in a month here. What survives is the instrument the level stood in for: **one dated future against spot**,
  where convergence is contractual and the entry basis is the return -- +195 to +382 bps annualised on the six-sample
  run, and the venue itself publishes 1.5 bps to rest and 3.5 to cross on a dated future with 25x maximum leverage,
  which is a 4 % initial margin a leg. What is left to measure is the path, not the price: the June 2026 inversion at
  -186 bps annualised, the margin on both legs, and the spot leg's venue.

- **`--parent` on the best-bid-offer path, and the two things that path gets wrong.** The futures venue publishes
  percentage bands rather than a price ladder, so an execution run there reads the touch, which is all a crossing
  child needs. Two consequences. That path keeps the venue's millisecond event time where the book path carries
  microseconds, so the strategy's deadline, the measurement's deadline and the reported `span_seconds` are converted
  at the call site and the unit is named in the parent spec. And its touch callback carries no timestamp at all, so a
  time-sliced `--algo` would silently emit one child: the mode now refuses anything but `--algo=market` instead of
  half-running. The specification is also looked up by symbol rather than fixed to BTCUSDT, with the positional
  quantity scale as an override of the symbol's own lot step, so one unit is 0.001 BTC, 0.1 XRP or one DOGE.

## The measured line

The line that adds the queue model, the fee model, the two latency legs, the order trace and the queue
distribution, the strategy sweeps, and the four-tier gate. Its entry point is the
[Design Review](docs/DESIGN_REVIEW.md), and every number it quotes is re-derived by
`bash tools/verify_l2.sh fast`.

### Highlights

- **Queue-aware fills.** Four queue models from a conservative lower bound to an optimistic upper bound,
  plus an arrival share, so a fill depends on the queue in front of a quote rather than on its price.
- **Fees charged on the instrument's face value**, after a model that charged price times quantity on an
  inverse contract overstated every fee by about 630 times and turned a strategy comparison into a
  measurement of the overcharge.
- **Two latency legs with opposite signs.** A late arrival quotes wider and fills less; a late cancel leaves
  a stale quote standing and is the leg that costs money.
- **Mechanism visibility:** a per-order trace, and a queue distribution over every order. On the mainline
  window the median quote never came within 3.9% of the front of its queue, and every order that reached the
  front traded.
- **Strategy sweeps:** quote width, requote threshold, order size and inventory limit, plus a four-strategy
  comparison with enough fills to read. The fee-to-edge ratio does not move across any of them.
- **A four-tier gate:** unit tests, pinned invariants and documented numbers, a full-file output hash, and an
  AddressSanitizer/UndefinedBehaviorSanitizer tier.

## v1.0-demo — the frozen demo

Tagged `v1.0-demo`, and kept for reproducing the earlier results and as a regression target. The simple
line: Binance Trade+BBO replay, legacy CSV and UDP inputs, the L1 strategy set, and an L1 fill that is a
price event because that path models no queue.
