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
