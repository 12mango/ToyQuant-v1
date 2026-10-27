# Project Brief

One-page handoff for another engineer or AI. Read this first; then the [Design Review](DESIGN_REVIEW.md) for
the argument, [Architecture](ARCHITECTURE.md) for the modules and state machine, and [Roadmap](ROADMAP.md) for
what is deliberately next.

## What it is

ToyQuant is a deterministic C++20 L2 replay and market-making simulator. One path goes from a market-data
file to an order book, a strategy decision, a matching engine with a queue model and fees, execution reports,
and a portfolio/PnL summary. A paper-trading `ws` mode reuses the same downstream pieces against a local
websocket feed. The same command produces the same output, and `tools/verify_l2.sh fast` re-derives the
documented numbers.

## What it is not

It is not an exchange client, a production trading system, or a profitable strategy. There is no exchange
connectivity, authentication or TLS client in the demo path; no persistence or restart recovery; no market
impact or queue feedback from the strategy's own size; no portfolio or multi-symbol risk yet. The risk layer
is an in-process pre-trade pipeline, not a broker or exchange risk gateway.

## What works today

| Area | What a demo can show | Main entry point |
|---|---|---|
| Replay modes | legacy CSV/UDP, Binance Trade+BBO, Deribit L2 replay, and the local `ws` paper path | `src/app/application.cpp`, [User Guide](USER_GUIDE.md) |
| L2 book and queue | price-time priority, four queue models, partial fills, self-trade prevention | `src/exchange/matching_engine.*` |
| Strategies | L1/L2 market makers (`passive_l2`, `inventory_aware_l2`, `flow_aware_l2`, `active_l2`) and the non-market-making `trend_l2` line | `src/strategy/*` |
| Pre-trade risk | kill switch, fat-finger band, token-bucket rate, exposure cap and position gate; a reducing order is allowed through | `src/app/risk_pipeline.*`, [Architecture](ARCHITECTURE.md) |
| Order state | Pending, Resting, PartiallyFilled, Filled, Cancelled, Rejected; reject/cancel reasons; pending release; invalid-transition and orphan counts | `src/app/order_ledger.*` |
| Measurement | risk-control check, trend parameter sweep, replay/direct/threaded equivalence, 200k and 2M slices | `tools/risk_check.sh`, `tools/trend_sweep.sh`, `tools/verify_l2.sh` |
| Faults | broker reject/partial/drop/outage, order-gateway timeout and retry, recorded stream replayed byte for byte | `src/lines/paper_broker.*`, `src/lines/order_gateway.*` |

## Demo boundary (freeze)

This is the line for the interview demo. **Inside** the boundary:

* deterministic replay and local paper trading;
* the L2 queue model, fees, latency and execution reports;
* the risk pipeline and the order state machine;
* `trend_l2` as a measured non-market-making example, including its negative result;
* the measurement tools, the gate and the documents.

**Outside** the boundary, deliberately:

| Deferred | Why it is out of the demo |
|---|---|
| real venue adapter, authentication and TLS | needs credentials and changes the story from simulation to operations |
| multi-symbol, portfolio construction and netting | a separate risk/portfolio project; one symbol is enough to show the loop |
| persisted order state, restart recovery, timeout query/reconcile | a live-system concern; the in-memory state is complete for a replay |
| cancel/replace, IOC/FOK, post-only and full venue semantics | exchange-specific and not needed for the state-machine lesson |
| market impact, queue feedback from own size, realistic taker fills | a research project; the current 100% touch-fill assumption is stated, not hidden |
| out-of-sample/Sharpe/capacity study | useful later; the current sweep is explicitly one window |
| block/SIMD parsing, shared-memory IPC, binary telemetry, config hot reload | performance or production concerns with no measured demo bottleneck |

During the freeze, the only accepted changes are: a bug fix on this path, a test or document that makes an
existing behaviour easier to see, or a demo script. A new strategy, venue, risk control or performance
mechanism requires an explicit decision to lift the freeze.

## Demo complete, for this stage

The demo is complete when:

1. `ctest --test-dir out/build/linux-debug` is 26/26;
2. `bash tools/verify_l2.sh fast` is tier 0 and tier 1 OK;
3. one short L2 replay, one `tools/risk_check.sh` run and one `tools/trend_sweep.sh` run can be shown;
4. this brief and the linked documents still describe the binary;
5. no open bug is hidden in the demo path. Known limitations are listed above and in the
   [Design Review](DESIGN_REVIEW.md), rather than being treated as future work that changes the demo.

## Commands

```bash
cmake --preset linux-debug
cmake --build out/build/linux-debug -j 8
ctest --test-dir out/build/linux-debug --output-on-failure
bash tools/verify_l2.sh fast          # cuts the 200k slice on demand
bash tools/risk_check.sh              # four pre-trade control runs
FAST_SLOW="5:30 10:60 20:120" ENTRIES="1 2 3" STOPS="4 6 8" bash tools/trend_sweep.sh
```

## Measured headlines

* A two-hour OKX paper session: 5,704,944 frames, zero sequence gaps, four forced reconnects and a
  reconciliation that stayed `ok`; the engine's recording replayed field for field.
* The non-market-making sweep is negative in all 27 cells. Its best cell (fast 5 / slow 30, 3-tick entry,
  4-tick stop) has 106 fills, net -6.04 USD and 5.30 USD of fees on one 200k-row window. That is a
  measurement, not a claim of alpha.
* The first risk measurement produced 33,685 rate-limit refusals for four submissions; the rejection callback
  and the exponential backoff reduced that to 15. A 1 bp fat-finger band went from 161 refusals to 13.
* The gate has 26 CTest binaries and runs tier 0 and tier 1 in `fast`, with the full-file hash and the
  sanitizer tier available through `all`.

## Where to read next

| Question | Document |
|---|---|
| Why is it built this way, and what is wrong with it? | [Design Review](DESIGN_REVIEW.md) |
| What module owns what, and how does an order move? | [Architecture](ARCHITECTURE.md), especially **Order state machine** |
| What is planned, and what is deliberately not? | [Roadmap](ROADMAP.md) |
| How do I run a mode or a flag? | [User Guide](USER_GUIDE.md) |
| In what order should I read the source? | [Main Line](MAINLINE.md) |
