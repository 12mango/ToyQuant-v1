# Architecture

ToyQuant is a small event-driven market-making simulator. It connects market-data input, a simplified order book, strategy decisions, order matching, execution reports, and CSV output. This page explains the implementation by source file; [User Guide](USER_GUIDE.md) contains the commands, scenarios, and experiment workflow.

## How to read this document

| You want to understand... | Start here |
|---|---|
| End-to-end event flow | [System Map](#1-system-map) |
| Input formats and validation | [Shared Types and Input Boundary](#2-shared-types-and-input-boundary) |
| Object ownership and callbacks | [Application Coordinator](#3-application-coordinator) |
| External book versus strategy orders | [OrderBook](#4-orderbook-external-market-view) and [MatchingEngine](#5-matchingengine-orders-queues-and-reports) |
| How strategies make decisions | [Strategy](#6-strategy-the-main-decision-module) |
| PnL and reusable metrics | [Backtest and Performance Metrics](#7-backtest-and-performance-metrics) |
| What the engine costs and how that is measured | [Performance](PERFORMANCE.md) |
| Why a latency-critical design is built the way it is | [Low-Latency Design](LATENCY_DESIGN.md) |

The diagrams show control flow. The tables show ownership and responsibilities. The code blocks
show the smallest representative implementation surface; detailed prose explains only the design
decisions that are not obvious from those views.

## For a reviewer in a hurry

The four things this document is worth reading for, in the order an engineer would ask about them:

* **the queue**, because it is the part most simulators fudge: a quote's fills are decided by what is ahead of it, the
  model carries four settings from a conservative lower bound to an optimistic upper bound, and the recorded bound sweep
  shows the conclusions of this repository do not move between them;
* **the fee**, because it is charged the way a broker charges it: a per-unit commission, a maker and taker rate, a floor
  per order instance, the fees that follow a sale's proceeds, and a credit for displayed liquidity — with the equity
  schedule that made a one cent tick visible as a cost rather than as an assumption;
* **the state machine**, because the order audit keeps what a strategy did rather than what it intended: filled order
  instances with their size histogram, cancels before and after a fill, the queue consumed, and the markout of the fills
  that arrived;
* **the honesty of the measurement**, because the numbers quoted in [Performance](PERFORMANCE.md) and
  [Performance History](PERFORMANCE_HISTORY.md) are re-derived by `tools/verify_l2.sh` rather than copied, and the
  designs that were measured and then rejected are kept rather than deleted — [Low-Latency Design](LATENCY_DESIGN.md) is
  that record.

## 1. System Map

```mermaid
flowchart LR
    CSV[CsvFeed\nCSV rows] --> T[Legacy Tick]
    UDP[UdpFeed\nUDP packets] --> T
    T --> LT[legacy::TickPipeline]
    BA[Market data adapter\nBinance or future formats] --> RF[ReplayFeed\ntime merge]
    RF --> MEV[MarketEvent\nTrade, BBO, or L2 snapshot]
    LT --> P[Pipeline::process_legacy_tick]
    MEV --> PE[Pipeline::process_event]
    PE --> OB
    PE --> ME
    P --> OB[OrderBook\non_tick / top]
    P --> ME[MatchingEngine\nmarket tick]
    OB --> S[Strategy]
    S -->|quotes and cancels| P
    P -->|limit orders| ME
    ME -->|ExecutionReport| S
    ME -->|Trade reports| TR[trades.csv]
    P -->|submitted orders| OR[orders.csv]
    TR --> BT[legacy::BacktestDriver]
    T --> BT
```

The v2 central path is intentionally short:

```text
MarketEvent -> OrderBook / MatchingEngine -> Strategy -> orders -> reports -> output
```

```mermaid
sequenceDiagram
    participant D as Data reader
    participant P as Pipeline
    participant V as Market view
    participant S as Strategy
    participant M as MatchingEngine
    D->>P: MarketEvent
    P->>V: update external state
    V-->>P: TopOfBook / L2MarketView
    P->>S: strategy decision
    S-->>P: orders + cancel requests
    P->>M: cancel / submit
    D->>M: external market trade
    M-->>S: ExecutionReport
    M-->>P: fills, fees, lifecycle
```

CSV and UDP remain compatibility inputs for the original Tick model. They enter through the
`legacy` boundary and are not the v2 market-data contract.

The source map is:

| Responsibility | Main files | Key types or functions |
|---|---|---|
| Shared data | `src/common/types.h`, `src/market/market_event.h` | `Side`, `ExecType`, `MarketEvent` |
| Instrument rules | `src/common/instrument_spec.h` | `InstrumentSpec`, `btc_usdt_spec` |
| Legacy input | `src/market/csv_feed.*`, `udp_feed.*`, `src/legacy/*` | `legacy::Tick`, `legacy::TickPipeline`, `legacy::tick_csv` |
| Market replay | `src/market/market_data_adapter.*` is the router, and each venue has its own file beside it: `binance_readers.*`, `deribit_readers.*`, `okx_readers.*`, sharing `field_parser.h`; plus `replay_feed.*`, `market_event_recorder.*`, `threaded_l2_replay.*` | `IMarketEventReader`, `ReplayFeed::run`, `MarketEventRecorder`, `ThreadedReplayFeed` |
| Hand-off between threads | `src/concurrency/spsc_ring.h` | `SpscRing::try_push`, `SpscRing::try_pop`, `LatencyHistogram` |
| Coordination | `src/app/application.*`, `src/app/pipeline.*` | `Pipeline::process_event`, legacy adapter, output callbacks |
| Market view | `src/orderbook/orderbook.*`, `l2_orderbook.*`, `tick_ladder.*` | `TopOfBook`, `OrderBook::market_top`, `L2OrderBook`, `TickLadder` |
| Matching | `src/exchange/matching_engine.*` | `MatchingEngine::match`, `send_order` |
| Strategy | `src/strategy/strategy.h`, `src/strategy/market_maker.h`, `src/legacy/l1_market_maker.h`, L2 strategy headers | Shared API, frozen L1 compatibility family, active L2 family |
| Legacy analysis | `src/backtest/backtest_driver.*` | `legacy::BacktestDriver::run`, `print_report` |
| Runtime logging | `src/utils/logger.*` | `Logger::log`, `Logger::error` |

## 2. Shared Types and Input Boundary

**Files:** `src/market/market_event.h`, `src/legacy/tick.h`, `src/legacy/tick_csv_parser.*`, `src/market/csv_feed.*`, `src/market/udp_feed.*`

The legacy CSV and UDP feeds produce the original `Tick` contract:

```cpp
namespace legacy
{
struct Tick
{
    uint64_t ts = 0;
    std::string symbol;
    double price = 0.0;
    uint64_t size = 0;
    Side side = Side::Unknown;
};

using TickCallback = std::function<void(const Tick&)>;
}
```

The legacy CSV and UDP paths retain this contract. Trades+BBO replay and L2 snapshot input use a
richer boundary:

```cpp
using MarketEvent = std::variant<MarketTrade, BboQuote, MarketDepthSnapshot, IncrementalBookBatch>;
```

`IMarketEventReader` isolates vendor schemas from replay. The Binance readers map aggregate trades
and book ticker rows into these domain events; `ReplayFeed` only performs streaming timestamp
merge. To support another vendor, implement readers for its files and add a factory branch in
`make_market_data_readers` without changing `Pipeline`, the strategy, or matching code.

Every file backed reader obtains its rows from `LineReader` (`src/market/line_reader.*`). It reads
1 MiB blocks, finds line boundaries with `memchr`, and returns a `std::string_view` into the block,
which removes both the per-character scan of `std::getline`/`gzgets` and the per-row `std::string`
allocation. The view is only valid until the next call, so a reader that must hold a row across
`next()` calls copies it into its own buffer. [Performance](PERFORMANCE.md) records the isolated
line-layer measurement and the end-to-end effect, and [Low-Latency
Design](LATENCY_DESIGN.md#1-the-line-layer-linereader) explains why it is faster.

Each v2 event carries an optional `exchange` identity. Binance L1 readers set it to `binance`,
while the Deribit L2 snapshot reader preserves the source column. `Pipeline` locks to the first
non-empty exchange and rejects later events from another exchange. This prevents Binance `BTCUSDT`
L1 parameters from being silently combined with Deribit `BTC-PERPETUAL` L2 data.

Before dispatch, `MarketDataValidator` rejects structural errors: non-positive or non-finite
prices, zero quantities, unknown trade sides, crossed BBO, timestamp regression, and non-increasing
per-stream sequence numbers. Cross-stream conditions are reported rather than rejected: trades
without a prior BBO, trades using a BBO older than one second, and trades more than 5 basis points
from the latest BBO midpoint. The replay `Pipeline` also filters trades without a usable BBO or beyond
the 5-basis-point deviation threshold before sending them to the strategy and matching engine,
preventing invalid alignment from creating synthetic fills.

`InstrumentSpec` is shared by the market-data adapter, strategy, order book, and matching engine.
Fee rates are carried in the specification and injected into the replay matching engine. The
current Deribit BTC perpetual benchmark assumes a 0.02% maker fee and 0.05% taker fee; these are
simulation assumptions, not account-tier guarantees.
For BTCUSDT it defines a `0.10` price tick and `1000000` integer quantity units per BTC. The replay
strategy therefore uses `0.001 BTC` base orders, a two-tick base spread, and a `0.1 BTC` inventory
limit.

`InstrumentSpec::unit_notional_usd` states what one quantity unit is worth in USD when the traded
price does not say it. Zero means the price *is* the unit value, which is right for an instrument
quoted in quote currency per unit: one BTCUSDT unit is one BTC and is worth `price`. A nonzero value
marks a contract with a fixed USD face value. Deribit perpetuals quote USD per contract while one
contract is worth a fixed 10 USD, so BTC-PERPETUAL sets 10 and a fill of one contract is 10 USD
rather than roughly 6300 USD.

Both the fee model and `Portfolio` read this field, so they cannot disagree about what a fill is
worth:

- `MatchingEngine::report_trade` charges `unit_notional_usd * quantity * rate`, so one
  BTC-PERPETUAL contract pays 0.02% of 10 USD.
- `Portfolio` values cash and positions through `position_value(unit, quantity, entry, price)`. For
  a price based unit this reduces to `quantity * price`, which is the spot case. For a fixed face
  value it is `unit * quantity * price / entry`, because the position holds a fixed USD exposure
  whose base amount was acquired at the entry price. That is the inverse contract relationship, so
  a one contract long gains exactly `10 USD * (price / entry - 1)`.

Before `unit_notional_usd` existed, both the fee and the cash balance were computed as
`price * quantity`. At 2020 BTC prices that overcharged a Deribit contract by roughly 630x, so
every L2 fee and every L2 cash and equity figure was meaningless. BTCUSDT and the legacy `Tick`
paths leave the field at zero and are unaffected.

Execution reports then carry the liquidity role and fee for each MarketMaker fill; the current
offline backtest can consume those recorded fees while retaining a fee-rate fallback for legacy
six-column trade files.

`InstrumentSpec::fee_per_share` is the second half of the same idea for the first instrument where the
fee is not a fraction of the notional. A US equity broker charges cents a share, and a rate cannot say
that: half a cent a share is 0.5 basis points on a hundred dollar share and 5 basis points on a ten
dollar one, so the cost of a fill depends on the share price in a way a fraction of notional cannot
represent. `us_equity_spec` therefore states a one cent tick, one quantity unit per share, and a
commission per share, and it leaves the maker and taker rates at zero, because that is how an equity
fill is billed. It is registered for the three symbols this repository has measured on a US venue --
SPY, and the screen's AAPL and TSLA -- **by name rather than by rule**, because registering a symbol is
a claim that its numbers are re-derivable and three named symbols keep that claim small. `fee_on_fill`
adds both terms rather than choosing between them, so an instrument can
carry a rate, a commission, both or neither, and every schedule that existed before the equity entry
leaves the new field at zero and is arithmetically unchanged.

The two terms differ in what they scale with, which is the reason both are kept: a rate charges for
size in dollars and a commission charges for size in shares, so a rate on a cheap instrument and a
commission on an expensive one are the two ways a venue can make a tight quote unprofitable. On a
hundred dollar share, one tick is a basis point and the tiered commission is a third of it, which is
the first fee in this repository that a tick can cover.

`InstrumentSpec::multiplier` is the third field of the same kind, for an instrument whose unit is
worth a multiple of its price. One micro E-mini S&P 500 contract is five dollars per index point, so
a unit that is one contract is worth `price * 5`, a quarter point tick is 1.25 USD, and a position of
one contract is notional of five times the price rather than the price. Without the field the fee and
the cash balance would both be five times too small, which is the same class of error the fixed face
value was added to fix in the other direction. It defaults to 1 and multiplying by 1 is exact, so no
schedule that predates the futures entry moves.

That combination is the point of the futures entry: a tick worth 1.25 USD against a commission of
about sixty cents a contract is the first market measured here where a one tick quote can pay for
itself before the queue and the adverse selection are counted, which is the question the perpetuals
kept answering with 2.5 ticks of fee against 0.1 ticks of capture.

BBO events update the external top of book and trigger quoting. Trade events carry aggressor side
and drive matching. External Tick/BBO state is stored separately from local strategy orders.
`market_top()` exposes only venue prices through `IOrderBook`. Strategy orders are owned only by
`MatchingEngine`; there is intentionally no local or combined view in `OrderBook`, so strategy code
cannot accidentally include its own orders in the market reference.

`L2OrderBook` is a separate multi-level view for `MarketDepthSnapshot` events. It replaces the
snapshot atomically, validates ordering and sequence, and derives an `L2MarketView`. The L2 replay
path passes that view explicitly to the selected strategy after venue and instrument validation;
raw vendor rows never enter strategy code, and Binance L1 data is not combined with Deribit L2.

A replay is delivered as fast as the file reads unless the run asks for pacing. `ReplayPacing` maps the file's
timestamps onto the wall clock, optionally at a speed, and `L2ReplayFeed` reports how late each delivery was
against that schedule: this process being busy, the reader being slow and the operating system not waking the
thread are all inside that number, which is why it is the feed-side figure a live session watches. The clock and
the sleep are injectable, so the schedule is asserted exactly in a test instead of waited for, and one real-time
case proves the default sleep is wired. Pacing changes no decision, only when the decision is taken, so the
`[PNL]` fields of a paced run are the same as the unpaced one and a non-zero fifth positional argument, which was
a fixed sleep per event, now means the same thing at real time.

The incremental L2 path uses `IncrementalBookBatch` events. The reader groups rows sharing a
`local_timestamp`, skips buffered updates before the first snapshot, and preserves snapshot-batch
reset boundaries. Each update sets an absolute level amount; amount zero removes the level. After
the batch is applied, the same `L2MarketView` interface is emitted, so strategies do not depend on
whether the source was a reconstructed snapshot or an incremental feed.

A real incremental message carries one or two level changes, so `IncrementalUpdates` keeps the first
four inside the event object and only moves to a vector when a batch grows past that. On the Deribit
incremental replay that removes one allocation and one free from nearly every batch. `begin()` and
`end()` still expose a single contiguous range, so range-for, `std::any_of` and `operator[]` behave
as they did when the storage was a plain vector. The same reader resolves each CSV column position to
a role once at construction, so the per-row loop switches on a jump table rather than comparing
against eight member indices per field.

`MarketEventRecorder` (`src/market/market_event_recorder.h`) reads the same boundary backwards: it
taps the delivery callback and writes every event into the two replay shapes, so a paced replay or a
live feed becomes a file the same binary reads again. A price is written as the shortest decimal that
parses back to the same double and a quantity as an integer, a batch's rows carry the batch's own two
timestamps, which is the field the reader regroups on, and the interleaving of the two streams is
re-derived from the timestamps by the rule the replay used the first time. What the formats cannot
express is refused rather than approximated: a trade with no aggressor side, a field holding the
separator, a batch whose updates disagree about their local timestamp, a snapshot whose level count
changes, or a stream that carries whole-book snapshots and incremental batches at once. The readers'
own sequence ids for a snapshot and a batch are the one field a recording does not reproduce, because
no column holds them. `bash tools/verify_l2.sh fast` records the 200k slice, replays the recording,
and requires the two runs to report the same numbers.

The live path's threading is implemented against the replay first, because the boundary is the same one
and a file reader is easier to hold still than a socket. `ThreadedReplayFeed`
(`src/market/threaded_l2_replay.*`) runs the feed on a thread of its own and hands each event to the
strategy thread over `concurrency::SpscRing` (`src/concurrency/spsc_ring.h`), a bounded
single-producer/single-consumer ring whose two indices sit on separate cache lines and whose
publication is a release store paired with an acquire load. The consumer runs on the calling thread, so
the strategy, the book and the portfolio keep the thread they always had and only the reader moves.
[Low-Latency Design section 6](LATENCY_DESIGN.md#6-the-hand-off-between-two-threads-spscring) prices the
primitive against the alternatives and
[the performance reference](PERFORMANCE.md#39-the-hand-off-between-two-threads) carries the table, measured
by `ring_bench` (`src/bench/ring_bench_main.cpp`) rather than asserted: the hand-off is a couple of
nanoseconds of mechanism and the event's own move dominates it.
Three decisions are worth stating because each has a cheaper-looking alternative that this path does
not take. A full ring is backpressure, not a drop: the feed waits for room and counts the wait, because
a replay that lost an event would not be the run anybody asked for, and the pacing line is where the
wait then shows up as a feed that fell behind its schedule. The consumer spins with a pause instruction
rather than sleeping on a condition variable, because at these rates the wake-up costs more than the
wait it replaces; the reported occupancy is what would say otherwise. And the two threads do not share
a `StageProfiler`: it accumulates on whichever thread calls it, so `--feed-thread` with
`--profile-stages` is refused rather than reported as a race.

What the hand-off costs is measured rather than described. Each event is stamped when the feed hands it
over, when the strategy thread takes it, and when the consumer returns, and the three differences are
fed to a `LatencyHistogram` (`src/utils/latency_histogram.h`, the same power-of-two-microsecond
convention the pacing lateness report uses). `run()` prints them as a `[LATENCY]` line: queue wait,
processing, and the total, each with a mean, a p50, a p99 and a maximum, plus the deepest the ring got
and how often the feed waited for room. The processing figure is where the decision and the orders the
synchronous engine accepted inside it both land, which is why there is no separate order leg here: the
venue's leg is what `--order-latency-us` models. The threaded path reports the same run as the direct
one, and tier 1 of `tools/verify_l2.sh` asserts that by comparing every number except the `[LATENCY]`
line itself.

The broker layer is a decorator over that engine rather than a second execution model. `PaperBroker`
(`src/exchange/paper_broker.*`) implements `IMatchingEngine`, takes over the venue's report callback and
forwards every call and every report, so the pipeline, the strategy, the book and the portfolio are
unchanged and a broker with no faults configured produces the run without it. What it adds is what a venue
does not do: refusals for reasons a strategy cannot see in the price, each counted because a refusal emits
no report — the same reason `IMatchingEngine::rejected_orders()` exists at all; a fill delivered as two
prints with the fee split the same way, which is fee- and position-neutral by construction so only the
report count moves; a report withheld on the way out while the venue's statement still records the
execution; and an outage that holds orders and delivers them in submission order when the venue returns,
answering a cancel for an order still held because that order is gone for good. Each fault is a count of
orders or fills rather than a drawn probability, so a faulted run is reproducible from its command line.
The broker also keeps a statement of its own, symbol by symbol, and `reconcile_positions`
(`src/accounting/reconciliation.h`) compares it against the portfolio's positions, gross traded quantity,
fill count and fees, which is the check the strategy already runs against the engine extended one layer
out. Cash is deliberately outside that comparison: the broker's USD arithmetic would be a second
implementation of the instrument model, and a copy that agrees because it was copied is not a check.

Levels are held in `TickLadder`, a flat array indexed by price tick. A `std::map` access descends
about ten pointer levels, and a sorted vector moves several kilobytes per insertion once a book holds
a thousand levels; Deribit BTC-PERPETUAL keeps more than a thousand live levels per side across a
span of about twenty thousand ticks, so both structures pay for that shape, and indexing by tick
turns a level change into one computed address. The range is learned from the levels themselves and
the array grows when an update arrives outside it, so no price window is configured and no update is
dropped. Iteration starts from a remembered best slot that is repointed when the touch is emptied,
which keeps a top-of-book walk inside the few populated slots near the touch. `L2OrderBook` keeps the
same interface, so `Pipeline`, the strategies and the replay tests are unchanged.

The validator enforces monotonic exchange and local timestamps and counts snapshot/reset batches
as reconnect diagnostics. The current normalized CSV schema does not expose Deribit
`prev_change_id`, so a true exchange sequence-gap check is intentionally not claimed; a future
adapter can add that field without changing the strategy interface.

The matching engine also retains the latest BBO liquidity for aggressive strategy orders. A buy
limit at or above the best ask executes at the ask; a sell limit at or below the best bid executes
at the bid. Execution is capped by the displayed BBO quantity, which is consumed across subsequent
orders until the next BBO update. Any unfilled remainder enters the existing local limit-order book.

`CsvFeed::run` reads rows, converts them into `Tick`, and invokes `cb_(t)`. The parser lives under
`src/legacy`, and `legacy::TickPipeline` is the explicit compatibility boundary before the old
Tick processing path. Malformed rows are reported and skipped.

`UdpFeed` has a different transport but the same legacy output contract. On Linux, `recvmmsg` receives
datagrams in batches. The parser uses `std::string_view` and `find`, then publishes parsed ticks
through a fixed-size ring buffer:

```cpp
size_t h = head_.load(std::memory_order_relaxed);
size_t next = (h + 1) % RING_SIZE;

if (next != tail_.load(std::memory_order_acquire))
{
    ring_[h] = t;
    head_.store(next, std::memory_order_release);
}
```

The producer publishes the tick before release-storing `head_`; the consumer acquire-loads the
index before reading the slot. This is a single-producer/single-consumer atomic queue. A full ring
drops the incoming tick by design.

## 3. Application Coordinator

**Files:** `src/app/application.*`, `src/app/pipeline.*`

`main.cpp` is only the process bootstrap. `Application` wires concrete objects together. The
`LegacyCsv` and `LegacyUdp` modes create a legacy Tick adapter, `Replay` creates a Trades+BBO
event feed, and `L2Replay` creates a trade+depth feed. `Pipeline` owns the v2 domain sequence; the
application intentionally does not introduce separate runner abstractions yet.

```mermaid
sequenceDiagram
    participant F as Feed
    participant P as Pipeline
    participant B as OrderBook
    participant M as MatchingEngine
    participant S as Strategy
    F->>P: process_event(MarketEvent)
    P->>B: on_bbo(BboQuote)
    P->>M: process_market_trade(MarketTrade)
    P->>B: top(symbol)
    B-->>P: TopOfBook
    P->>S: on_top_of_book(top)
    S-->>P: new orders
    P->>M: cancel_order / send_order
    M-->>S: ExecutionReport
    M-->>P: write trades.csv
```

The report callback is the other important application boundary:

```cpp
engine_.set_report_callback(
    [this](const ExecutionReport& report)
    {
        strategy_.on_order_update(report);
        write_trade_csv_row(trades_out_, report);
    });
```

The `[this]` lambda is a C++11 closure. `Pipeline` stores references to its collaborators rather than owning them; the run function constructs them in an enclosing scope and destroys them after the feed finishes. `std::unique_ptr<Strategy>` expresses exclusive ownership of the selected concrete strategy, while `std::atomic<uint64_t>` supplies the order-ID counter.

`orders.csv` is written when the pipeline submits a candidate order, before the matching result is known. `trades.csv` receives only `Trade` reports owned by `MarketMaker`; `Resting`, `Filled`, and `Cancelled` are lifecycle events, not additional executions. New trade files append `liquidity_role` and `fee` columns; the six required legacy columns remain unchanged for compatibility.

### 3.1 Dependency injection at the coordinator boundary

`Pipeline` does not construct the order book, strategy, matching engine, or output streams itself. They are created by the application mode and passed into the constructor:

```cpp
auto output_files = open_output_files(source);
OrderBook order_book;
auto strategy = make_strategy(cfg.strategy_name);
Logger logger(to_abs_path("logs/toy_quant.log"));
MatchingEngine engine(&logger);
Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine, logger);
```

The constructor receives the domain collaborators through their interfaces and stores references to them:

```cpp
Pipeline(std::ofstream& orders_out, std::ofstream& trades_out, IOrderBook& order_book,
                 Strategy& strategy, IMatchingEngine& engine, Logger& logger)
    : orders_out_(orders_out),
      trades_out_(trades_out),
      order_book_(order_book),
      strategy_(strategy),
            engine_(engine),
            logger_(logger)
{
    // Install the report boundary after the collaborators are available.
}
```

This is constructor injection: the `Pipeline` declares what it needs, while the caller chooses the concrete implementations. `IOrderBook` and `IMatchingEngine` make the coordinator depend on behavior rather than on `OrderBook` and `MatchingEngine` details. The strategy is injected through the `Strategy` base class, so the same pipeline can run `NaiveMarketMaker`, `OptimizedMarketMaker`, or `L1MarketMaker`:

```cpp
std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name)
{
    if (strategy_name == "naive")
    {
        return std::make_unique<NaiveMarketMaker>(100, 0.00001);
    }
    if (strategy_name == "l1")
    {
        L1MarketMakerConfig config;
        config.order_size = 100;
        config.base_spread = 0.000003;
        return std::make_unique<L1MarketMaker>(config);
    }
    return std::make_unique<OptimizedMarketMaker>(100, 0.000003);
}
```

Dependency injection keeps construction at the application boundary and the event sequence in
`Pipeline`. Interfaces and callbacks also give focused tests substitution points without requiring
the complete application.

### 3.2 RAII and lifetime-bound resources

The application uses RAII, or Resource Acquisition Is Initialization, to bind cleanup to object lifetime. In the legacy CSV mode, the local objects are created in dependency order and remain alive while the feed invokes the pipeline:

```cpp
auto output_files = open_output_files(source);
OrderBook order_book;
auto strategy = make_strategy(cfg.strategy_name);
Logger logger(to_abs_path("logs/toy_quant.log"));
MatchingEngine engine(&logger);
Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine, logger);
legacy::TickPipeline tick_pipeline(pipeline, order_book, order_book, engine);
CsvFeed feed(csv_file, [&](const legacy::Tick& tick) { tick_pipeline.process(tick, true); }, cfg.delay,
             &logger);
feed.run();
```

When `run_legacy_csv_mode` returns, destruction runs in reverse declaration order. The feed, pipeline,
logger, strategy, order book, and output streams clean themselves up through object lifetime. The
same cleanup occurs during stack unwinding if the feed throws.

The locking code applies the same rule to synchronization:

```cpp
TopOfBook OrderBook::market_top(const std::string& symbol)
{
    std::lock_guard<std::mutex> lock(mtx_);
    // Read the protected book state and return a snapshot.
}
```

Constructing `lock` acquires `mtx_`; leaving the function releases it automatically. The same RAII
pattern protects order-book updates, while `std::unique_ptr<Strategy>` owns the selected strategy.

### 3.3 Unified runtime logging

**Files:** `src/utils/logger.h`, `src/utils/logger.cpp`

`Logger` owns the optional text log file and mirrors each message to the appropriate console stream. `log(...)` writes to `stdout`; `error(...)` writes to `stderr`. When an application supplies a file path, the constructor creates its parent directory, opens the file in truncate mode for the current run, and `write` copies every message to that file. An empty path preserves console output without opening a file.

The application entry points own the `Logger`: CSV and UDP runs use `logs/toy_quant.log`, while `backtest_main` uses its configurable backtest log path. They pass it to the components that emit runtime events. `Pipeline` and `BacktestDriver` require `Logger&`, because their lifetime is always within the entry point's logging scope. `CsvFeed` and `MatchingEngine` accept an optional `Logger*`, preserving standalone construction for narrow tests and other callers:

```cpp
Logger logger(to_abs_path("logs/toy_quant.log"));
MatchingEngine engine(&logger);
CsvFeed feed(path, callback, delay, &logger);
```

This is dependency injection rather than a global logger. Market data, matching, and backtesting describe events but do not decide file locations, create directories, or manage file streams. `orders.csv` and `trades.csv` remain separate: they are structured business records consumed by the backtest, not human-readable runtime logs.

`Logger` uses a variadic template so callers can stream several values without manually constructing a temporary string:

```cpp
template <typename... Args>
void log(Args&&... args)
{
    write(format(std::forward<Args>(args)...), std::cout);
}
```

`typename... Args` declares a template parameter pack: at each call, the compiler deduces zero or more argument types. For example, `logger.log("order=", id, " qty=", quantity)` deduces types for the string literals and numeric values. `Args&&...` is a forwarding-reference parameter pack, and `std::forward<Args>(args)...` preserves whether each argument was an lvalue or rvalue when passing it to `format`.

Inside `format`, the fold expression `(stream << ... << args)` expands the pack into chained stream insertions. The example above is conceptually `stream << "order=" << id << " qty=" << quantity`. The resulting `std::string` lets the non-template `write` method implement the destination policy only once. The template definitions remain in the header because the compiler must see their bodies when it instantiates them for each argument combination.

## 4. OrderBook: External Market View

**Files:** `src/orderbook/orderbook.h`, `src/orderbook/orderbook.cpp`

`OrderBook` contains external market data only. Legacy Tick input updates simplified bid or ask
price maps, while Trades+BBO replay replaces the latest venue BBO snapshot.

```cpp
struct SideBook
{
    std::map<PriceTick, uint64_t, std::greater<PriceTick>> external_bids_qty;
    std::map<PriceTick, uint64_t> external_asks_qty;
    BboQuote external_bbo;
    bool has_external_bbo{false};
};
```

For legacy input, the comparators make the first bid the highest price and the first ask the lowest.
For v2 replay, `market_top()` directly returns the latest valid `external_bbo`. The mutex protects
market updates and snapshots. Local strategy orders do not enter this object.

### 4.1 Single ownership of strategy orders

`MatchingEngine` is the single source of truth for working strategy orders:

```cpp
std::unordered_map<std::string, MEOrderBook> books_;
std::unordered_map<uint64_t, exchange::Order*> order_index_;
```

Each price level owns FIFO orders in a `std::list`, so pointers in `order_index_` remain stable until
that order is erased. Submission, partial fills, fills, and cancellation mutate this one structure;
execution reports notify the strategy of lifecycle changes. The former mirrored private
`OrderBook` state was removed to eliminate synchronization drift.

### 4.2 Price representation

External inputs, strategy calculations, CSV records, and reports use `double` for readability.
Before a price is used as an order-book key or in matching comparisons, the implementation converts
it to an integer tick:

```cpp
using PriceTick = int64_t;
std::map<PriceTick, uint64_t, std::greater<PriceTick>> bids_qty;
```

```cpp
inline PriceTick to_price_tick(double price)
{
    return static_cast<PriceTick>(std::llround(price / PRICE_TICK_SIZE));
}
```

The matching engine uses the same `PriceTick` keys for price-time priority and cancellation.
`to_price()` converts ticks back to `double` only at output boundaries. Integer ticks make level
identity, comparison, and ordering deterministic while retaining readable public records.

## 5. MatchingEngine: Orders, Queues, and Reports

**Files:** `src/exchange/order.h`, `execution_report.h`, `matching_engine.*`

The exchange namespace defines the order vocabulary. `Order` carries identity, symbol, side, limit or market type, original quantity, remaining quantity, timestamp, and owner. `ExecutionReport` adds the event type and reports either a fill or a lifecycle transition.

```cpp
struct ExecutionReport
{
    uint64_t order_id{};
    exchange::Side side{};
    ExecType exec_type{};
    std::string symbol;
    double price{};
    uint64_t quantity{};
    uint64_t ts{};
    std::string owner;
};
```

The main matching rules are:

- External ticks become `Market` orders, so they consume strategy liquidity but are never rested.
- Each price level uses FIFO order and matches the best executable price first.
- Owner normalization prevents `MarketMaker` from trading with itself.
- A public trade that prints at the stored best bid or ask reduces that stored best size, so a
    later taker order cannot fill against displayed liquidity that has already traded. This is
    separate from the queue model and applies on every replay path.
- In L2 replay, a maker order submitted at the displayed best price joins behind the displayed
    top-level quantity. Subsequent market trades consume this `external_queue_ahead` before local
    FIFO orders become eligible to fill. Ordinary L1 replay keeps its existing BBO semantics, so an
    L1 maker fill is not charged any queue position.

The matching algorithm is a direct price-time implementation:

```cpp
auto best_ask_it = book.asks.begin();
double best_price = best_ask_it->first;
if (new_order.price < best_price) break;

Order& resting = ask_queue.front();
uint64_t traded = std::min(qty, resting.remaining);
qty -= traded;
resting.remaining -= traded;
```

The engine emits `Trade` reports for executions, then `PartialFill` or `Filled` for the resting
order. An unfilled incoming limit order receives `Resting` and enters the appropriate queue.

```mermaid
flowchart TD
    I[Incoming order] --> V{Valid?}
    V -- no --> X[Ignore submission]
    V -- yes --> P{Crosses best opposite price?}
    P -- no --> R[Rest limit order]
    P -- yes --> Q[Take front order at best price]
    Q --> ST{Same normalized owner?}
    ST -- yes --> C[Cancel incoming order]
    ST -- no --> F[Emit two Trade reports]
    F --> L{Quantity remains?}
    L -- yes --> Q
    L -- no --> D[Emit Filled or PartialFill]
```

Owner normalization removes whitespace and lowercases characters. A self-trade cancels the
aggressor. `Trade.quantity` is executed quantity; lifecycle quantities are remaining quantity.

### Per-symbol state is keyed by an integer id

The engine keeps its state in per-symbol containers: `books_`, `external_bbo_`, `l2_top_bbo_`, the two
last-trade clocks, and the set of symbols that have an L2 top. A replay touches one symbol millions of
times and reaches about ten of those containers per event, so keying them by the symbol string meant
hashing a thirteen character key ten times per event for a value that never changes.

`symbol_id()` resolves a symbol to a small `SymbolId` once per entry point and the containers are keyed
by that id instead. The public interface still takes strings, so nothing outside the engine changed:
`process_bbo` resolves once and hands the id to the private `process_bbo_for`, which is why the body of
`process_bbo` is separate from the override. `find_symbol()` is the lookup-only variant, used where a
rejected event must not register a new symbol.

[Low-Latency Design](LATENCY_DESIGN.md#3-the-engines-per-symbol-state-symbol-ids) explains the
mechanism and records the measured effect on the `engine` stage.

## 6. Strategy: The Main Decision Module

**Files:** `src/strategy/strategy.h`, `src/strategy/market_maker.h`, `src/legacy/l1_market_maker.h`

The strategy is the decision layer, not the matching layer. It sees `TopOfBook`, keeps its own working-order and position state, and returns candidate `StrategyOrder` values. The pipeline later assigns IDs and converts them to `exchange::Order`.

```cpp
class Strategy
{
   public:
    virtual ~Strategy() = default;
    virtual std::vector<StrategyOrder> on_top_of_book(
        const std::string& symbol, const TopOfBook& tob) = 0;
    virtual void on_order_submitted(const StrategyOrder& order) = 0;
    virtual std::vector<uint64_t> cancel_requests() = 0;
    virtual int64_t net_position() const = 0;
    virtual size_t working_order_count() const = 0;
    virtual void on_order_update(const ExecutionReport& report) = 0;
};
```

`main.cpp` selects an implementation with `std::make_unique`; the pipeline then uses the common
interface without knowing which strategy is active. The virtual destructor makes polymorphic
ownership safe.

### Strategy lifecycle at a glance

```mermaid
flowchart LR
    MV[Market view] --> D[Strategy decision]
    D --> Q[Candidate quotes]
    D --> C[Cancel requests]
    Q --> P[Pipeline assigns IDs]
    C --> P
    P --> E[MatchingEngine]
    E --> R[ExecutionReport]
    R --> S[Strategy state update]
    R --> A[Portfolio and metrics]
```

| Callback | Direction | Responsibility |
|---|---|---|
| `on_top_of_book` / `on_l2_market_view` | Pipeline -> strategy | Produce the next candidate quote set |
| `on_market_trade` | Pipeline -> strategy | Update aggressor-flow state |
| `on_order_submitted` | Pipeline -> strategy | Record exchange-assigned working orders |
| `cancel_requests` | Strategy -> pipeline | Return stale or unsafe order IDs |
| `on_order_update` | MatchingEngine -> strategy | Apply fills, cancellations, and partial fills |

The strategy chooses intent. The pipeline owns sequencing and IDs. The matching engine owns actual
working orders. This separation is the key invariant for every strategy family below.

### Strategy family map

| Family | Input view | Main purpose | Execution model |
|---|---|---|---|
| Legacy | `Tick` | Compatibility and simple demonstrations | Legacy tick pipeline |
| L1 replay | `TopOfBook` + trades | BBO-aware quoting | Trades+BBO matching |
| L2 replay | `L2MarketView` + trades | Depth-aware quoting and queue diagnostics | Queue-aware L2 matching |

### 6.1 NaiveMarketMaker: baseline quote

The naive strategy requires both sides of the top of book, computes the midpoint, places one quote on each side, and rounds both prices to the configured tick size:

```cpp
double mid = (tob.bid_price + tob.ask_price) / 2.0;
double buy_price = std::round((mid - base_spread / 2.0) / tick_size) * tick_size;
double sell_price = std::round((mid + base_spread / 2.0) / tick_size) * tick_size;

orders.push_back(StrategyOrder(Side::Buy, symbol, buy_price, base_order_size, 0));
orders.push_back(StrategyOrder(Side::Sell, symbol, sell_price, base_order_size, 0));
```

It does not request cancellations, so working orders can accumulate. It updates position from
`Trade` reports and serves as a deliberately simple baseline.

### 6.2 OptimizedMarketMaker: stateful quoting

The optimized strategy adds controls in a deliberate order:

```mermaid
flowchart LR
    T[TopOfBook] --> M[Midpoint]
    M --> W[deque midpoint window]
    W --> SM[Smoothed midpoint]
    SM --> TR[Simple trend]
    SM --> SP[Spread adjustment]
    TR --> Q[Three quote levels]
    SP --> Q
    INV[Position + working exposure] --> Q
    Q --> O[Buy/sell StrategyOrder]
    O --> C[Refresh and cancel policy]
```

`OptimizedMarketMaker` is the stateful strategy. It keeps a window of recent midpoints in a
`std::deque`, averages that window, and estimates short-term trend from the change in the smoothed
midpoint. It then builds up to three quote levels. The core price adjustment is:

```cpp
double raw_buy = smooth_mid - level_spread / 2.0 - std::max(0.0, trend) -
                 inv_spread_bias * (inventory > 0 ? 1.0 : 0.0);
double raw_sell = smooth_mid + level_spread / 2.0 + std::max(0.0, trend) +
                  inv_spread_bias * (inventory < 0 ? 1.0 : 0.0);

double buy_price = std::round(raw_buy / tick_size) * tick_size;
double sell_price = std::round(raw_sell / tick_size) * tick_size;
```

Inventory is `position + working_exposure`, with buys positive and sells negative. If inventory is
long, the strategy makes buys less attractive and reduces buy size, encouraging future sells. If
inventory is short, it applies the symmetric adjustment to sells. Once the limit is exceeded, the
risky side is disabled.

The refresh policy separates “should quote now?” from “which old orders must be cancelled?”:

- `quote_age_ticks` forces refresh after an age limit.
- `quote_refresh_ticks` compares the smoothed midpoint with `last_quote_mid`.
- `cancel_requests()` returns active IDs when a quote is stale.
- `on_order_submitted()` records IDs and resets quote age.

In short: observations create quotes, submitted IDs become working state, execution reports update
quantities and position, and stale quotes are cancelled before replacement.

### 6.3 L1MarketMaker: BBO-aware quoting

L1 strategies are frozen compatibility implementations. Their CLI names remain available for
reproducing earlier L1 replay and benchmark results; new strategy work belongs to the L2 path.
The archived implementations are in `src/legacy/l1_market_maker.h`. Their behaviour is pinned rather than described: `bash tools/verify_l2.sh fast` checks the L1
invariants on every edit and `bash tools/verify_l2.sh all` compares a full-file run byte for byte,
so a change in those makers that moves the numbers is a change to a recorded result. Execution
quality is not computed inside them; `ExecutionQualityMetrics` in the pipeline owns edge, markout,
quote lifetime and inventory for every strategy, and prints them in `[EXECUTION]`.

#### The two gates

Two different things are called a gate in this project, and they fail in opposite directions. The first refuses a
change that would move a recorded number; the second refuses a quote that cannot pay for itself.

The verification gate is `tools/verify_l2.sh`, and it has four tiers because the checks cost different amounts and
catch different mistakes. Tier zero runs the unit tests, because a failing test once shipped in a commit that the
older gate could not see. Tier one replays a two hundred thousand record slice and compares the event counts,
orders, fills, realized PnL and consumed queue position against the values the documents quote, re-derives every
`toyquant:check` block, including the ones a benchmark binary carries rather than the main one, and lints the prose
for a flag the code accepts that no document mentions. Tier two replays
1.5 GB and compares the hash of the whole output, which is what makes a claim about timing defensible. Tier three
builds with the address and undefined behaviour sanitizers and runs the tests and a slice through them, because a
leak in the block reader or an out-of-range slot in the ladder is invisible in a normal build. A tier that skips
says so on its own line, because a hidden skip and a pass are indistinguishable in a log.

The decision gate lives in the strategy and exists because the fee is larger than the edge at the touch. It has two
knobs and both are off at zero, which is what keeps every recorded run reproducible. `--edge-cover-ticks` requires
the edge a fill would capture, measured from the reference price in ticks, to cover the maker fee plus the ticks it
names, where the fee in ticks is `rate * price / tick_size`: 2.55 ticks on the Deribit contract at 2020 prices and
about 186 on the OKX contract at 2026 prices, which is the ratio that stops the policy being tuned into profit.
`--min-spread-ticks` refuses the situation rather than the price: when the top of book is narrower than it asks for,
nothing is placed at all, because a one tick book leaves the touch half a tick from the midpoint and those are the
fills the bucketed markout shows resolving at or below zero. Both refusals are counted, so a run can report how
often the decision layer declined to act and not only what it did.

One difference matters when reading L1 results: **the L1 path models no queue in front of a quote.** Its book
has no depth-derived levels, so a resting L1 order starts at the touch with nothing ahead of it and a print
through its price fills it. The L2 path fills only after the queue ahead has been consumed. Fill counts on
the two paths therefore answer different questions, and every queue-model result in
`docs/PERFORMANCE_HISTORY.md` comes from the L2 path.

`L1MarketMaker` is the replay-oriented single-level strategy. It consumes BBO updates for quoting
and venue trades for order-flow information. Its behavior is intentionally layered so each risk
control has a distinct purpose:

1. **Input filtering.** A trade is used for imbalance only when a latest BBO exists, the trade is
    not older than the BBO, its BBO age is below `max_market_trade_age`, and its price deviation
    from the BBO midpoint is below `max_trade_deviation_bps`. Direct one-argument calls remain
    available for focused tests; the replay pipeline uses the BBO-aware overload.
2. **Flow and volatility state.** A single FIFO window of `TradeSample` values tracks buy and sell
    aggressor volume. A rolling window of absolute midpoint changes estimates short-term movement.
    This is a simple educational volatility proxy, not a statistical volatility model.
3. **Reservation price.** Effective inventory is `position + working_exposure`, where working buys
    are positive and working sells are negative. The reservation midpoint is shifted by a bounded
    `tanh(inventory / inventory_limit)` term: long inventory shifts the center down to discourage
    more buying, while short inventory shifts it up to encourage buying back.
4. **Quote width.** The effective spread is at least the larger of the configured base spread and
    current market spread, plus twice the average midpoint change. Trade-flow and BBO-size imbalance
    add an adverse shift to both sides. Prices are rounded to the instrument tick and clipped to
    remain passive relative to the venue BBO.
5. **Quote size.** Volatility stress reduces both sides toward a configurable minimum quantity
    ratio. Inventory reduces only the risky side. Above `inventory_risk_threshold`, the risky side is
    disabled and only the inventory-reducing side remains.
6. **Lifecycle.** A changed quote first requests cancellation; replacement waits for cancellation
    reports. The strategy counts what it owns: submitted quantity, fills, cancels, quotes, quote
    refreshes and the queue it consumed. Execution quality is not among them; see below.

The tunable values are grouped in `L1MarketMakerConfig`. The legacy positional constructor remains
for compatibility, while the application entry point uses the named configuration object. The
configuration is deliberately in-process rather than JSON/YAML: this project is an educational
simulator, not a production configuration service.

`StrategyMetrics` carries only what a strategy owns, so the runtime summary can report L1-specific
counters without downcasting the strategy: submitted quantity, fills, cancels, quotes and quote
refreshes. Execution quality is deliberately not included. `captured_edge` is an immediate midpoint
comparison and `adverse_selection` is a later markout after five quote cycles; both belong to
`ExecutionQualityMetrics` in the pipeline, which computes them once for every strategy from the same
reports and the same market events, and prints them in `[EXECUTION]`. The L1 makers used to keep a
second copy of those quantities from their own view of the mid, and it was deleted rather than
reconciled because the two could disagree and nothing compared them. Neither number includes fees or
constitutes a complete portfolio PnL calculation.

### 6.4 ActiveL2MarketMaker: L2 mainline

The L2 strategy receives an explicit `L2MarketView` from `L2OrderBook`. The view contains the
top of book, micro-price, aggregated depth, depth imbalance, symbol, and snapshot timestamp. L2
strategy state is separate from the L1 `OrderBook`; it never consumes raw vendor rows.

```mermaid
flowchart TD
    V[L2MarketView] --> F[Fair price]
    V --> I[Depth imbalance]
    T[Recent aggressor trades] --> F
    T --> X[Flow toxicity]
    P[Position] --> R[Inventory reservation]
    F --> R
    I --> R
    X --> G[Quantity and side gates]
    R --> G
    G --> Q[Tick-rounded passive quotes]
    Q --> L[Refresh / cancel lifecycle]
    L --> M[Queue-aware matching]
```

| Production setting | Value | Used for |
|---|---:|---|
| Signal mode | `Flow` | Combines micro-price, depth, and trades |
| Trade window | `16` events | Recent aggressor-flow estimate |
| Imbalance shift | `2 * tick_size` | Depth contribution to fair price |
| Trade shift | `1 * tick_size` | Flow contribution to fair price |
| Refresh threshold | `2` ticks | Repricing trigger |
| Maximum quote age | `50` L2 decisions | Stale-quote fallback |
| Toxicity threshold | `0.65` | Disables the threatened side |
| Weak-flow threshold | `0.9` | Activates extreme-flow protection |
| Volatility alpha | `0.25` | Active-L2 EWMA smoothing |
| Pause threshold | `9` ticks/second | Extreme-volatility risk gate |

#### 6.4.1 Fair-price construction

`L2MarketMaker` supports `Baseline`, `Depth`, `Micro`, and `Flow` signal modes. The production
factory selects `Flow` with a 16-trade FIFO window. In that mode the fair price is:

```text
fair = micro_price
    + depth_imbalance * imbalance_shift
    + trade_imbalance * trade_imbalance_shift
```

`depth_imbalance` is the normalized bid-versus-ask quantity across the configured visible levels.
`trade_imbalance` is the normalized aggressor buy-versus-sell volume in the recent trade window.
The micro-price uses top-level quantities to move the reference toward the side with less displayed
liquidity. These signals shift the reservation price; they do not authorize crossing the spread.

#### 6.4.2 Inventory and toxicity controls

The base engine computes:

```text
inventory_ratio = clamp(position / inventory_limit, -1, 1)
inventory_shift = inventory_ratio * base_spread
```

Long inventory moves the reservation price down and makes additional buys less attractive; short
inventory applies the symmetric behavior. At the hard inventory limit, the risk-increasing side is
disabled.

For flow toxicity, the configured threshold is `0.65`. A strongly positive flow disables the sell
side; a strongly negative flow disables the buy side. When flow magnitude exceeds `0.9` while
inventory is already materially imbalanced, weak-flow protection halves both quote quantities and
moves both prices one tick farther from the center. The name `weak_flow` refers to weakening the
quotes, not to weak market flow.

#### 6.4.3 Passive quote construction and lifecycle

Prices are tick-rounded around the fair price, inventory shift, and weak-flow shift. A final passive
clip enforces:

```text
bid <= external_best_bid
ask >= external_best_ask
```

This keeps the L2 strategies maker-only. When orders are working, a fair-price move of two ticks
requests cancellation. The active mainline counts quote age only when the external top-of-book
midpoint moves by at least one tick; depth-only updates do not consume quote lifetime. This avoids
treating incremental feed granularity as market movement. The fallback maximum age remains 50
market-price decisions. The pipeline processes cancellation before allowing replacement, and the
active strategy records price refreshes, age refreshes, and risk pauses separately.

This behavior is specifically important for incremental input: many updates change displayed
quantity without moving the best price. Counting every such update as quote age creates artificial
refresh churn. The retained rule removes that data-resolution artifact without changing the fair
price, inventory, toxicity, or passive-price logic.

The pipeline also reports consumed queue volume back to the strategy. If a quote reaches its age
limit while the queue ahead is actively being consumed and fair price has not changed, L2 retains
the quote and resets its age instead of cancelling and losing time priority. A price change or risk
pause still takes precedence. The preservation is bounded to three holds before a stale quote must
be reconsidered. This is a queue-preservation rule, not a profitability assumption.

#### 6.4.4 Active risk overlay

`ActiveL2MarketMaker` composes `L2MarketMaker`; it does not duplicate its inventory or flow logic.
It maintains a time-normalized midpoint-movement EWMA:

```text
volatility = alpha * normalized_move + (1 - alpha) * previous_volatility
```

The production value is `alpha = 0.25`. Ordinary volatility does not widen quotes. When the EWMA
reaches `9` ticks per second, active L2 requests cancellation of all working orders and pauses new
submissions. This is a tail-risk gate, not a continuous spread signal. The historical
`adaptive_l2` name is accepted only at the factory boundary and constructs this same class.

For both snapshot and incremental batches, the EWMA uses exchange event time so strategy signals are
normalized against market time rather than file or network processing cadence. Incremental
`local_ts` remains available in `L2MarketView` for future latency diagnostics, but does not alter
the current trading decision. The same market period can still produce different quote-cycle counts
because incremental data contains more update batches; that execution-resolution difference must be
measured before comparing PnL.

#### 6.4.5 Execution and queue model

Before L2 strategy decisions, `Pipeline::process_l2_market_view` sends the snapshot top of book to
`MatchingEngine::process_l2_top`. If a maker order joins the displayed best price, the displayed
quantity is stored as `external_queue_ahead`. Subsequent market trades consume that quantity before
the local FIFO order. This prevents a new order from receiving queue-first fills.

The engine exposes `queue_ahead_consumed` in the run summary. Queue-clear diagnostics count price
levels whose external queue reaches zero; they do not measure cleared quantity. Fees are calculated at execution time
from `InstrumentSpec`; the current Deribit BTC perpetual benchmark assumes maker `0.02%` and taker
`0.05%`. The L2 replay is therefore evaluated on net PnL, fees, maker/taker role, queue consumption,
markout, inventory, and refresh-cause metrics together.

The matching engine supports four queue interpretations, and they are one family rather than four
opinions. When a level's displayed size falls by `r` with no trade at that price, the only unknown is how
much of that `r` was ahead of a resting order, and each model is a claim about that one random variable,
`X(r)`, with `q` the share of the display that is ahead of the order:

| model | `X(r)` | mean | variance |
|---|---|---|---|
| `Conservative` | `0` | `0` | `0` |
| `ProRata` (default) | `r q` | `r q` | `0` |
| `Lumpy` | `(r / k) Binomial(k, q)` | `r q` | `r^2 q (1 - q) / k` |
| `Optimistic` | `r` | `r` | `0` |

The first and last bracket the **mean**, and `k` moves only the **variance**, which is why `ProRata` is
the neutral default rather than a compromise. `tools/calibrate_queue_model.py` measures this feed to put
99.96% of a level's size decrease on cancel or amendment rather than trade, so a queue that only moves
when a trade prints contradicts the mechanism; the cancellation sizes it measures, where a median
decrease removes 74% of a median level and the removed fraction is flat across four orders of magnitude
of level size, are what makes `k = 1` the lumpiest honest reading. The queue arithmetic tests in
`tests/matching_engine_test.cpp` pin the consequences: at `q = 1` every model but the conservative one
removes the whole decrease, and the lumpy mean matches pro-rata over 200 replications. The arrival rule
is separate and is the share `alpha` in `Q0 = alpha D0`, one for a FIFO venue; it is a knob, but a
second-order one, because `q` is a fixed point of the pro-rata dynamics and only trades, 0.04% of what
moves a level, break it. `docs/PERFORMANCE_HISTORY.md` carries the derivations and the measurements.

The C++ replay path supports both source types. The Python benchmark reconstructs a compact top-five
timeline from grouped incremental updates so execution and markout fields use the same report shape
for both sources. This is a top-five comparison timeline, not an order-level queue reconstruction.

The current active L2 implementation is intentionally a conservative research mainline. Its
18-window in-sample baseline and four-window 2020-05-01 sample-out-of-time check remain negative
after fees. The strategy is structurally complete for this simulator, but those results do not
establish live profitability.

Every L2 replay records its queue model. `prorata` is the default and the mainline result: it is the
neutral assumption about the one thing the data cannot show, which is where in a level's queue the
cancellations sit. `conservative` (nothing comes off the queue ahead) and `optimistic` (all of it
does) bracket it, and the measured cancel share says `conservative` reads as a floor that is harsher
than the mechanism. Both are models for information lost by aggregated L2 data, not proof of live fill
probability. A strategy comparison is useful only when its behavior remains reasonable across
multiple windows, dates, and queue models.

The L2 replay applies a fixed one-market-event cancellation delay. Legacy CSV/UDP and ordinary
snapshot replay use the configured delay path; this delay is event-count based, not milliseconds.
Every replay mode marks the portfolio to market at the last market price before reporting, so
`equity` and `unrealized_pnl` account for any position still open when the input ends. The L1
replay path used to skip that step, which left both fields at zero while `cash` was populated.

The run summary also records `audited_orders`, `audited_filled_orders`,
`audited_cancelled_orders`, `cancelled_after_fill_orders`, `cancelled_before_fill_orders`, and
`total_order_lifetime_cycles`. The last field counts quote decision cycles, not elapsed time.

### 6.5 Strategy families and comparison

For the same `sample_ticks.csv` run, the two legacy Tick strategies diverge immediately:

| Strategy | Submitted orders | Submitted quantity | Cancel requests | Fill rate | Net position | Current equity | Max drawdown |
|---|---:|---:|---:|---:|---:|---:|---:|
| `naive` | 38 | 3800 | 0 | 0.231579 | -880 | 999.286800 | 0.000717 |
| `optimized` | 24 | 1379 | 9 | 0.31037 | -428 | 999.670190 | 0.000334 |

The table uses an initial capital of `1000.0`. It shows the core difference: `naive` is more aggressive and leaves more working orders behind, while `optimized` submits less, cancels stale quotes, and ends with smaller adverse position and drawdown. That is why the optimized version is the better demonstration of stateful market making in this project.

`L1MarketMaker` is evaluated on the Trades+BBO replay path rather than this legacy Tick-only
comparison. Its additional runtime measurements are useful for explaining quote safety and
execution quality, but they are not yet folded into the offline PnL report.

The Trades+BBO replay path has an L1 strategy family. These variants share the same `Strategy` interface,
instrument scale, matching engine, and fee model, so the comparison isolates decision behavior:

| Strategy | Main idea | Strength | Limitation | Role in this demo |
|---|---|---|---|---|
| `passive_l1` | Fixed two-sided L1 quotes with basic inventory limits | Simple and easy to explain | Does not use trade flow or volatility | Passive baseline |
| `inventory_aware_l1` | Inventory-dependent price and quantity skew | Demonstrates risk-aware quoting | Can still trade too much in a one-sided market | Inventory baseline |
| `flow_aware_l1` | Adds recent aggressor-flow and BBO-size imbalance | Avoids some flow-against trades | Still a transitional experiment; may reduce activity too much | Flow experiment |
| `active_l1` | Inventory-reversion-first quotes with shorter quote lifetime | More visible activity and inventory rotation | More adverse-selection and inventory risk | Active experiment |
| `l1` | Combines inventory, volatility, flow, markout, refresh, and fee-aware spread controls | Most complete replay strategy | More mechanisms and metrics to interpret | Mainline demo strategy |

`PassiveL1MarketMaker` refreshes quotes when the midpoint, BBO, or quote age changes, but its
prices remain symmetric around the midpoint. Its inventory logic limits the risky side without
using a forecast of short-term direction.

`InventoryAwareL1MarketMaker` shifts the two quote offsets according to the signed inventory and
reduces the risky-side quantity. It is useful for showing the difference between a neutral maker
and one that actively controls exposure, but it does not retain a trade-flow window.

`FlowAwareL1MarketMaker` keeps a quantity-weighted FIFO window of recent aggressor trades. It
combines trade imbalance and displayed BBO imbalance using the current demo weights `0.6` and
`0.4`. When the signal is sufficiently directional, it moves the threatened quote one tick away
and reduces the threatened-side quantity. It is intentionally bounded: it is a learning strategy,
not a claim of production alpha.

The mainline `L1MarketMaker` retains the more mature inventory and volatility controls, then adds
the useful flow-aware behavior as a constrained overlay. It also applies a fee-aware spread floor:
the configured base spread is compared with a fraction of the theoretical two-sided maker-fee
break-even spread. The current demo uses a `0.40` coverage factor, so the strategy does not treat
the result as a production profitability guarantee. The factor is a demonstration control that
helps make the relationship between gross PnL, fees, and net PnL visible.

`ActiveL1MarketMaker` is deliberately separate from `L1MarketMaker`. It gives inventory reduction
priority: when long, it moves the bid farther away and keeps the sell quote comparatively active;
when short, it does the opposite. It also expires quotes sooner and uses a simpler fee-aware
spread. This makes inventory rotation and trade activity easier to observe, but it accepts more
adverse-selection risk. It is an experiment for studying the activity-versus-execution-quality
trade-off, not a replacement for the defensive mainline. A fill immediately marks the quote as
stale, so the next BBO cycle can cancel and rebuild around the updated inventory. Its benchmark
metrics include submitted and filled quantity, quote count, captured edge, fees, average and peak
inventory, inventory sign changes, and quote lifetime.

The L2 replay family uses the separate `L2MarketView` contract described in section 6.4:

| Strategy | Main idea | Role |
|---|---|---|
| `passive_l2` | Baseline depth-aware quoting | Reference baseline |
| `inventory_aware_l2` | Adds an outer inventory adjustment layer | Inventory experiment |
| `flow_aware_l2` | Flow/depth fair-price behavior | Signal comparison |
| `active_l2` | Flow L2 engine plus volatility risk gate | L2 mainline |
| `adaptive_l2` | Compatibility name for `active_l2` | Legacy alias |

Unlike the L1 family, L2 decisions receive depth snapshots and use the queue-aware matching path.
L2 comparisons therefore include `queue_ahead_consumed`, maker/taker role, side-specific markout,
and refresh-cause metrics in addition to PnL and inventory. The detailed L2 decision sequence and
execution boundary remain in section 6.4; this subsection only defines comparison roles.

For the current BTCUSDT replay demo, the benchmark prints `gross_pnl`, `net_pnl`, `fees`,
`fee_ratio`, filtered market trades, filled quantity, final position, captured edge, adverse
selection, inventory path, markout counts, and quote lifetime. These execution-quality metrics are
computed by `Pipeline`, so simple and complex strategies share the same measurement basis. Always read gross PnL and fees together: a strategy can have
more fills or a positive captured edge while still losing after fees.

The benchmark can be run with:

```bash
./out/build/linux-debug/strategy_benchmark \
    data/v2/test_aggTrades_5k.csv \
    data/v2/test_bookTicker_5k.csv BTCUSDT 1000000
```

The benchmark is an educational replay, not a live-trading validation. Its results depend on the
selected replay window, displayed BBO liquidity, fee assumptions, and the quality of the paired
Trades+BBO data.

## 7. Backtest and Performance Metrics

**Files:** `src/backtest/backtest_driver.h`, `backtest_driver.cpp`

`BacktestDriver` is an offline analysis component. It reads the Tick file and recorded trades,
replays positions with slippage, and writes a report. It does not participate in live matching.

```cpp
double exec_price = trade.price +
    (trade.side == Side::Buy ? slippage_ : -slippage_);
double fee = trade.quantity * exec_price * fee_rate_;

auto& pos = positions[trade.symbol];
```

The PnL logic uses a **net-position** model: positive quantity is long, negative quantity is short,
and zero is flat. A buy first closes a short; a sell first closes a long; any remainder extends the
opposite position.

`Position` stores the average entry price. Closing quantity contributes to `realized_pnl`; remaining
quantity contributes unrealized PnL at the latest Tick price. The backtest uses an initial capital
baseline, records equity while replaying ticks and trades in timestamp order, and then computes
`max_drawdown` from that equity curve. The `orders_file` argument is retained for CLI compatibility
but is not read.

**Files:** `src/backtest/performance.*`, `tests/performance_test.cpp`

The reusable `metrics` module keeps formulas independent from the live pipeline and `BacktestDriver`.
The live pipeline uses execution metrics; the backtest uses maximum drawdown.

| Metric | Meaning | Current consumer |
|---|---|---|
| `fill_rate` | Filled quantity divided by submitted quantity | `Pipeline` execution summary |
| `cancel_rate` | Cancel requests divided by submitted orders | `Pipeline` execution summary |
| `inventory_exposure` | Absolute value of net position | `RunSummary` portfolio metrics |
| `max_drawdown` | Largest decline from an equity peak | `BacktestDriver` |
| `fees_paid` | Fees charged on recorded MarketMaker fills | Runtime and offline summaries |
| `adverse_selection` | Delayed fill markout accumulated after a quote horizon | L1 runtime summary |
| `average_abs_inventory` | Average absolute effective inventory at quote decisions | L1 runtime summary |
| `quote_lifetime` | Number of BBO decision cycles before fill or cancel | L1 runtime summary |
| `queue_ahead_consumed` | L2 market volume absorbed ahead of local maker orders | L2 runtime and benchmark summary |
| `buy_queue_ahead_levels_cleared`, `sell_queue_ahead_levels_cleared` | Number of price levels whose external queue reached zero | L2 runtime and benchmark summary |
| `audited_orders`, `audited_filled_orders`, `audited_cancelled_orders` | Order lifecycle terminal-state counts | Runtime summary |
| `cancelled_after_fill_orders`, `cancelled_before_fill_orders` | Cancelled orders split by whether they had a fill | Runtime summary |
| `orders_that_filled`, `filled_orders_by_size` | Filled order instances (a modification or a cancel and replace makes a new one) and their histogram in `count/quantity` buckets of `<=50, 51-99, 100-199, 200-499, 500-999, 1000+` shares, which is what a broker's minimum per order is charged on | Runtime summary |
| `min_commission_orders`, `min_commission_paid` | Orders charged a schedule's floor under one order's commission, and what that floor added. Zero unless `--fee-schedule=min=...` names one; the charge itself is booked to `fees_paid` and `cash` when the order ends, because that is when the order's total is known | Runtime summary |
| `max_exposure_limit_usd`, `hard_limit_rejections` | The pre-trade exposure cap in USD and the orders it refused. The cap marks the projected position at the order's own price, the conservative side of the mark for an order that adds to the position, and it runs before the position gate so that neither gate folds the other's pending quantity into its own check | Runtime summary |
| `kill_switch_loss_usd`, `kill_switch_triggered`, `kill_switch_ts`, `kill_switch_rejected_orders` | The loss the switch was set at, whether it tripped, the timestamp, and the orders the strategy asked for after it did and did not get. It is a latch, checked once a cycle in `Pipeline::submit_strategy_actions` before anything is submitted, so a strategy cannot reason its way past it; it stops quoting rather than flattening, because a control that loses money on the way out is a worse one than a control that stops, and the strategy's own cancels still run so that resting quotes come back on the next cycle | Runtime summary |
| `total_order_lifetime_cycles` | Sum of quote-cycle lifetimes for audited orders | Runtime summary |

The metric tests cover zero denominators, positive and negative inventory, empty equity curves, and a known peak-to-trough drawdown. This separation lets future metrics be added without expanding `backtest_driver.h` or duplicating formulas in `main.cpp`.

**Files:** `src/accounting/portfolio.*`, `tests/portfolio_test.cpp`

`Portfolio` books execution reports and values cash, realized PnL and equity. Until the carry line it
knew one income stream, the spread a fill captures, and one cost, the fee.
`accrue_interval_flow` adds the flow a *position* earns or pays for as long as it is held: funding on
a perpetual, the basis a future pays at roll, a dividend, or the financing a position is charged. One
method for all four, because the ledger side is identical in each and only the source of the rate
differs:

```cpp
double flow = portfolio.accrue_interval_flow(symbol, interval_rate, mark_price);
```

`interval_rate` is the rate one USD of notional earns per interval to a long position, so the
position's own sign decides the direction of the payment and one rate serves both sides of a market:
a short pays exactly what a long receives. A published funding rate is the rate longs pay and enters
as `-funding_rate`; a dividend is positive and financing negative. The notional is the USD value the
class already assigns the position at `mark_price` through `unit_notional_at`, so a fixed-USD-face
contract accrues on its face value and a price-quoted unit on `quantity * mark_price`; the entry price
and unrealized PnL play no part. `accrue_interval_flows` applies one interval across the whole book
from a rate map and a mark map, which is the shape a rate series has and the shape two legs will need,
and a position that either map does not name accrues nothing rather than being guessed at.

Cash, realized PnL and equity move together by the amount returned. An accrued flow has been paid
rather than marked, so it does not wait for a closing trade to become realized, and equity tracks cash
so the identity `mark_to_market` produced still holds between marks. `tests/portfolio_test.cpp` states
the cash, the equity and the realized PnL after N intervals for a price-quoted position, a fixed-face
contract and a short, pins the notional basis and the sign rule, and checks that a flat position, an
unknown symbol and an unpriced price-quoted instrument all accrue nothing. There is deliberately no
data plumbing yet: the mechanism is checkable before a rate series exists, the way the queue model was
checked before live fills were discussed. The order of the remaining steps is scoped in
[Roadmap](ROADMAP.md#the-carry-line-financing-flows-before-carry-strategies).

The rate then arrives from a file, which is `src/market/funding_rate.*`. Two shapes are recognised by
their own header: an OKX daily funding archive, which holds every swap on the venue in one file, and a
Binance Vision monthly archive, which holds one symbol. `FundingRateSeries` keeps one instrument's
settlements in order and consumes them forward, so a settlement is applied exactly once, at the first
event at or after its timestamp, on the position the previous event left and at the last price the run
has seen. The settlement instant itself is not in the event stream, so that mark is the nearest causal
one available; the position is exact, because no event between the settlement and the event that
follows it could have changed it. The columns are read as published and the polarity is flipped once,
here, because a published funding rate is what longs pay while `accrue_interval_flow` takes the rate a
long earns. The run summary prints a `[FUNDING]` line with the file's own arithmetic (points, span,
summed rate, annualized rate) next to what the replay did with it (settlements reached, USD moved), and
only when `--funding-rates` supplied a series, which is why a run without one still produces exactly
the output it produced before this input existed. A settlement reached before the account holds a
position is counted and moves nothing: the line therefore reports two different things, and a window
that begins after a settlement is the case where they disagree.

The two legs are then the carry mode itself, `src/accounting/carry_ledger.*`. A leg is a stated USD
notional rather than a contract count, so one quantity unit is one dollar: the venue's taker rate
applies to the number the command states, the funding accrues on the same number, and neither needs a
price feed. What that leaves out is stated rather than hidden — a fixed-contract position's notional
drifts with the mark, which is a second-order term in carry against fees, and the mode does not model
it. Both legs are opened and closed as taker fills at the published rates through the same
`fee_on_fill` the matching engine charges with, so a fill booked outside a book cannot be priced
differently from one booked inside it.

Two numbers describe the resulting book and they are not interchangeable. `carry_gross_notional_usd`
is both legs and `carry_net_notional_usd` is their difference, which is the account's own exposure: a
hedged carry holds two notional and is exposed to none of it, and a report offering only one of the two
would make the position look either twice its size or half its return. The acceptance number is the
annualized carry divided by the fees the position paid, which is the form the roadmap states, printed
next to `carry_breakeven_hours`, which is the same comparison in the form a decision needs: a fee is
paid once while a carry accrues every interval, so the two only meet through a holding period, and a
ratio above one can coexist with a window in which the position did not quite pay for itself.

