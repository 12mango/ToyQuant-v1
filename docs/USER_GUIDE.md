# HFT Demo User Guide

## 1. Purpose and Audience

HFT Demo is a small, reproducible C++ market-making simulation. Its purpose is to make a complete trading-system data path visible and approachable:

```text
Market data -> top of book -> strategy -> orders -> matching -> execution reports -> CSV output -> backtest
```

The project is designed for learners who want to read and modify a realistic-shaped system without needing exchange credentials, a production network stack, or a large framework. It is also useful as a compact reference when reviewing C++ techniques such as interfaces, callbacks, containers, RAII, atomics, and error handling.

This is not a production trading platform. It must not be connected to a live venue or used to make investment decisions.

## 2. What the Demo Does

The current implementation can:

- Replay market ticks from CSV.
- Receive ticks through a local UDP socket.
- Maintain a simplified top of book and local order state.
- Run either a naive or inventory-aware market-making strategy.
- Match local limit orders against synthetic external liquidity.
- Emit trade and lifecycle reports.
- Write orders and trades to CSV.
- Replay ticks and trades to calculate a basic PnL report.
- Run focused regression tests with CTest.

The implementation intentionally does not model full exchange behavior. It has no authentication, live exchange connectivity, FIX protocol, complete market depth, queue position, persistent recovery, risk gateway, or latency model.

## 3. Repository Map

```text
src/
  main.cpp                Application entry point and pipeline wiring.
  market/                 CSV and UDP market-data feeds.
  orderbook/              Local order state and top-of-book calculation.
  strategy/               Strategy interface and market-making implementations.
  exchange/               Orders, reports, and matching engine.
  backtest/               PnL replay executable and metrics.
tests/                    Regression tests.
data/scenarios/           Input market-data scenarios.
data/runtime/             Generated orders and trades.
tools/                    Scenario generator and UDP sender.
docs/                     Architecture, scenarios, and this guide.
```

Useful companion documents:

- [Architecture](ARCHITECTURE.md)
- [Market Scenarios](SCENARIOS.md)
- [Data Files](../data/README.md)

## 4. Build and Test

### 4.1 Requirements

- CMake 3.16 or newer.
- A compiler with C++20 support.
- Python 3 for the helper scripts.
- Linux or another POSIX-like environment for the current UDP implementation. The CMake presets also describe Windows and macOS configuration targets, but the UDP source currently includes POSIX socket headers.

### 4.2 Configure and build

From the repository root:

```bash
cmake -S . -B build
cmake --build build
```

This creates these executables under `build/`:

- `hft_demo`: CSV or UDP simulation pipeline.
- `backtest_main`: PnL replay application.
- `strategy_state_test`: strategy state regression test.
- `orderbook_state_test`: order-book state regression test.
- `matching_engine_test`: matching rule regression test.

CMake is configured for C++20. On non-MSVC compilers it enables `-Wall`, `-Wextra`, optimization, and `-march=native`. `-march=native` is convenient for a local demo but means a binary may not run on a different CPU family.

### 4.3 Run tests

Run all registered tests:

```bash
ctest --test-dir build --output-on-failure
```

Run one test directly after building its target:

```bash
cmake --build build --target matching_engine_test
./build/matching_engine_test
```

The tests use the standard library `assert` macro. Assertions can be disabled when `NDEBUG` is defined, so use the default/debug-style configuration when you want the tests to actively check behavior.

## 5. Running the Simulation

### 5.1 CSV mode

The general form is:

```bash
./build/hft_demo csv [path_to_csv] [ms_delay] [strategy]
```

Examples:

```bash
./build/hft_demo csv data/scenarios/flat_ticks.csv 0 optimized
./build/hft_demo csv data/scenarios/shock_ticks.csv 100 naive
```

Arguments:

| Argument | Meaning | Default |
|---|---|---|
| `path_to_csv` | A tick CSV file. | `data/scenarios/synthetic_ticks.csv` |
| `ms_delay` | Non-negative delay between processed ticks, in milliseconds. | `0` |
| `strategy` | `optimized` or `naive`. | `optimized` |

The application validates the mode, strategy, delay, CSV path, and UDP port before starting. Use `--help` to print the command summary:

```bash
./build/hft_demo --help
```

During CSV playback, the console prints one `[TICK]` line per input tick and a final `[SUMMARY]` line. A tick line shows the input event plus the current top-of-book values. The summary includes submitted order count and quantity, cancellation count, trade report count and quantity, fill rate, cancellation rate, net position, inventory exposure, and number of working orders.

### 5.2 UDP mode

The general form is:

```bash
./build/hft_demo udp <port> [strategy]
```

For example:

```bash
./build/hft_demo udp 9000 optimized
```

In another terminal, send a scenario:

```bash
python3 tools/udp_sender.py 127.0.0.1 9000 data/scenarios/sample_ticks.csv 0
```

The sender's positional arguments are optional and mean:

```text
host port file delay_ms
```

When `delay_ms` is omitted or zero, the Python sender sleeps according to the timestamp gaps in the CSV. When it is positive, it sleeps for that fixed duration after every datagram.

UDP mode runs until it is interrupted, normally with `Ctrl+C`. The current main loop is intentionally simple and continuously polls the feed when no tick is available. This keeps the demo easy to inspect, but it is not a production scheduling model.

### 5.3 Generated runtime files

Each `hft_demo` run creates or truncates:

```text
data/runtime/orders.csv
data/runtime/trades.csv
```

The runtime directory is created automatically. Failure to create it or open either output file stops the application with an error instead of continuing with partial output.

Do not run two simulations that write to the same runtime directory at the same time. Both processes would truncate and append to the same output files.

## 6. Data Contracts

### 6.1 Tick input

Tick scenarios have this CSV schema:

```csv
ts,symbol,price,size,side
1759080000000,EURUSD,1.18300,58,B
```

| Field | Type | Meaning |
|---|---|---|
| `ts` | unsigned integer | Timestamp in milliseconds. |
| `symbol` | string | Instrument identifier, for example `EURUSD`. |
| `price` | floating point | Tick price. |
| `size` | unsigned integer | Tick size. |
| `side` | `B` or `S` | Buy-side or sell-side market event. |

`CsvFeed` skips a header whose first value is `ts` or `timestamp`. It logs malformed rows and continues with the next row. In contrast, `BacktestDriver` is strict: malformed numeric values or invalid trade sides abort the backtest with a row number.

### 6.2 Order and trade output

Both output files use this schema:

```csv
ts,symbol,side,price,quantity,order_id
```

`orders.csv` records strategy orders before they are sent to the matching engine. `trades.csv` records only `Trade` reports owned by `MarketMaker`; it is the input used by `backtest_main`.

A single match emits a trade report for each side. Filtering on the owner prevents the external synthetic `Market` side from being written as a second duplicate trade in `trades.csv`.

## 7. The End-to-End Pipeline

`Pipeline` in `src/main.cpp` is the application coordinator. For every received `Tick`, it runs this sequence:

1. `OrderBook::on_tick` updates the external top-of-book view.
2. `MatchingEngine::process_market_tick` treats the tick as external liquidity and tries to fill local resting orders.
3. `OrderBook::top` returns the current best bid and best ask.
4. `Strategy::on_top_of_book` returns new quotes for the instrument.
5. `Strategy::cancel_requests` returns working order IDs to cancel before submitting new quotes.
6. The pipeline assigns monotonically increasing IDs, records each order, tells the strategy that it was submitted, and sends it to the matching engine.
7. Matching reports are delivered through a callback. The strategy updates its state and the pipeline records eligible trades.

The ordering matters. Existing orders can be filled by the current market event before the strategy creates its next set of quotes. Then the strategy sees the updated top of book and its own updated position before it decides what to do next.

## 8. Market Data Module

### 8.1 Shared types

`src/common/types.h` defines `Tick`, `Side`, `ExecType`, and `TickCallback`.

`TickCallback` is an alias for:

```cpp
using TickCallback = std::function<void(const Tick&)>;
```

It allows `CsvFeed` to receive any callable that accepts `const Tick&`, including a lambda that forwards data into `Pipeline::process_tick`.

### 8.2 CSV feed

`CsvFeed` owns a file path, callback, and optional delay. Its `run()` method reads rows sequentially, parses each row into a `Tick`, invokes the callback, and optionally sleeps.

This is intentionally a streaming parser rather than a loader that keeps all rows in memory. The trade-off is simple and appropriate for a demo: parsing is easy to follow, while CSV syntax beyond simple comma-separated fields is not supported.

### 8.3 UDP feed

`UdpFeed` opens a non-blocking UDP socket and receives packets on a background thread. On Linux it uses `recvmmsg` to receive a small batch of datagrams at once. Parsed ticks enter a fixed-size ring buffer; the main thread consumes them through `pop_tick`.

Datagrams must use the same five-field tick format:

```text
ts,symbol,price,size,side
```

Important current limitations:

- UDP delivery is unordered and lossy by design.
- When the ring buffer is full, incoming ticks are dropped.
- `start()` returns `false` when the socket cannot be created or bound, but `run_udp_mode` does not currently report that failure explicitly.
- The feed should be stopped before destruction in a future lifecycle cleanup; the current application is intended to be interrupted as a demo process.

These are useful discussion points when learning how a production feed handler differs from a compact demonstration.

## 9. OrderBook: Local State and Top of Book

`OrderBook` serves two related purposes:

1. It keeps local strategy orders and their lifecycle state.
2. It exposes a simplified top of book to the strategy.

The book stores bid and ask price levels separately. Bid maps sort high-to-low and ask maps sort low-to-high, so `begin()` identifies the best visible price on each side.

### 9.1 State lifecycle

The order state enum is:

```text
New -> Active -> PartialFilled -> Filled
                       |             
                       -> Cancelled
```

`Rejected` is returned when an ID is unknown to the state index. An order becomes `Active` when `add_order` accepts it. A partial fill decreases its remaining quantity; a full fill removes it from active order storage and marks it `Filled`. Cancellation removes it and marks it `Cancelled`.

`add_order` rejects zero IDs, zero quantities, inconsistent `remaining > qty`, and IDs already known to the state index. `apply_partial_fill` rejects overfills, so an invalid update cannot silently reduce an order below zero.

### 9.2 Simplified market view

`on_tick` writes the tick size at the tick price into the bid or ask quantity map. `top(symbol)` reads the first entry of each ordered price map.

This is not a full market-data book. A new external tick at a price replaces the stored quantity at that price, and the implementation does not remove old external levels automatically. Local strategy order quantities also use the same quantity maps. Therefore the returned top of book is a simplified teaching model, not an authoritative market-depth reconstruction.

## 10. Matching Engine

`MatchingEngine` owns price levels used for matching local orders. For each symbol it maintains:

```cpp
std::map<double, PriceLevel, std::greater<double>> bids;
std::map<double, PriceLevel> asks;
```

A `PriceLevel` contains `std::list<exchange::Order>`. The list is a FIFO queue: new resting orders are appended to the back and the matcher fills from the front.

### 10.1 Valid order submission

`send_order` accepts only orders with:

- A non-zero ID.
- Non-zero `qty` and `remaining`.
- `remaining <= qty`.
- An ID not already in the active matching index.

Rejected input is ignored by the current API; it does not emit a `Rejected` execution report. This is a deliberate simplification and a natural future extension.

### 10.2 Price-time priority

For a buy order, the engine starts at the lowest ask. It can trade only while the incoming price is at least that ask price. For a sell order, it starts at the highest bid and can trade only while the incoming price is at most that bid price.

Within a price level, it repeatedly fills the earliest resting order. The execution price is the resting level's price, not necessarily the incoming order's limit.

### 10.3 External ticks as liquidity

`process_market_tick` converts a valid tick into a synthetic market order owned by `Market`. The synthetic order is matched but never rests. This lets scenario ticks consume strategy orders without modeling a separate external matching venue.

### 10.4 Self-trade prevention

Before matching two orders, the engine normalizes both owners by removing whitespace and lowercasing characters. If the normalized values match, it prevents the trade and emits a `Cancelled` report for the incoming order's unfilled quantity.

For example, `" MarketMaker "` and `"marketmaker"` represent the same participant. Empty owners do not match under this rule.

The policy is intentionally simple: it cancels the aggressor rather than cancelling or decrementing the resting order. Real venues use different self-trade-prevention policies, so do not treat this as an exchange specification.

### 10.5 Execution reports

`ExecutionReport` carries an order ID, side, event type, symbol, price, quantity, timestamp, and owner.

Quantity has event-specific meaning:

- `Trade`: quantity executed by this report.
- `PartialFill`, `Filled`, `Resting`, `Cancelled`: quantity remaining on the order after the event.

For a match, the engine emits `Trade` reports for both participants. It then emits `PartialFill` or `Filled` for the resting order. When an incoming limit order receives at least one fill, it receives `PartialFill` or `Filled`; any unfilled remainder can then receive `Resting`.

Consumers should update positions from `Trade` reports. Lifecycle reports describe state but must not be counted as additional fills.

## 11. Strategies

`Strategy` in `src/strategy/strategy.h` is an abstract interface. `main.cpp` stores it through `std::unique_ptr<Strategy>`, so either concrete strategy can be chosen at runtime without changing the pipeline.

The interface separates quotation decisions from execution feedback:

- `on_top_of_book`: creates candidate orders.
- `on_order_submitted`: records an assigned order ID.
- `cancel_requests`: identifies orders to remove before the next quote cycle.
- `on_order_update`: consumes trade and lifecycle reports.
- `net_position` and `working_order_count`: expose summary state.

### 11.1 NaiveMarketMaker

The naive strategy calculates the midpoint:

$$
mid = \frac{bestBid + bestAsk}{2}
$$

It places one buy and one sell order around that midpoint using a configured spread and rounds prices to the configured tick size. It does not request cancellations, so working orders can accumulate during a long run. It is useful as a baseline for understanding the event flow.

### 11.2 OptimizedMarketMaker

The optimized strategy adds several teaching-oriented controls:

- A moving window of midpoint prices for smoothing.
- A simple trend estimate from changes in smoothed midpoint.
- Up to three quote levels.
- Wider spreads when the observed bid-ask gap is large.
- Inventory-aware price bias and quantity scaling.
- Quote aging and refresh thresholds.

It estimates working exposure from its open orders and combines it with filled position. When inventory is positive, it reduces buy size and biases buy prices lower; when inventory is negative, it reduces sell size and biases sell prices higher.

This is a simple inventory-control example, not a calibrated market-making model. In particular, the variable named `tick_vol` is the current bid-ask spread, not a statistical volatility estimator.

### 11.3 Strategy state updates

On `Trade`, both strategies find the order in `open_orders`, update position by executed quantity, and decrease the tracked remaining quantity. Buy trades add to position; sell trades subtract from position. `Cancelled` and `Filled` erase the order. `Resting` and `PartialFill` carry lifecycle information but do not directly change position.

## 12. Backtesting and Reproducibility

`backtest_main` reads tick and trade CSV files, then writes a report to a log file.

```bash
./build/backtest_main \
  data/scenarios/synthetic_ticks.csv \
  data/runtime/orders.csv \
  data/runtime/trades.csv \
  0 0 backtest logs/backtest.log
```

Argument order:

```text
backtest_main [tick_file] [orders_file] [trades_file] [slippage] [fee_rate] [mode] [log_file]
```

Current details to know:

- `tick_file` and `trades_file` must exist.
- Relative paths are anchored to the CMake project root.
- `orders_file` is accepted for CLI compatibility but is not currently read by `BacktestDriver`.
- The `mode` argument accepts `realtime` or falls back to `backtest`; the current driver runs the same CSV-based calculation for both values.
- The log parent directory is created automatically and log-open failures terminate the program.

### 12.1 PnL model

For each trade, the backtest adjusts execution price for slippage:

$$
executionPrice = tradePrice +
\begin{cases}
slippage, & buy \\
-slippage, & sell
\end{cases}
$$

Fee is:

$$
fee = quantity \times executionPrice \times feeRate
$$

When a buy closes a short position, realized PnL is:

$$
closedQuantity \times (averageShortPrice - executionPrice) - fee
$$

When a sell closes a long position, realized PnL is:

$$
closedQuantity \times (executionPrice - averageLongPrice) - fee
$$

Any residual quantity after closing the opposite position opens or extends a new position. Average price is weighted by quantity for same-direction additions. Final equity is realized PnL plus unrealized PnL marked against the final tick price for each symbol.

### 12.2 Why repeated runs are deterministic

At the start of every `BacktestDriver::run()`, the driver clears positions, last prices, realized PnL, and the equity curve. It also sorts symbols before writing per-symbol report lines. This avoids carrying state into repeated calls and avoids unordered-map iteration changing output order.

With the same input files, parameters, and log path, repeated backtest runs should produce identical log output.

Important limitation: the equity curve currently receives only the final equity value, so maximum drawdown is calculated over a one-point curve and is normally zero. A future version could append equity after each trade or tick to make drawdown meaningful.

## 13. C++ Concepts Used by the Project

### 13.1 Interfaces and runtime polymorphism

`Strategy`, `IOrderBook`, and `IMatchingEngine` are abstract interfaces. A class becomes abstract when it has pure virtual functions:

```cpp
virtual std::vector<StrategyOrder> on_top_of_book(...) = 0;
```

The application holds a `std::unique_ptr<Strategy>`, which can own either `NaiveMarketMaker` or `OptimizedMarketMaker`. This is runtime polymorphism: the program chooses a concrete implementation while calling through one stable interface.

The benefit is clear module boundaries. The cost is virtual dispatch and more indirection, both insignificant for this learning project.

### 13.2 RAII and `std::unique_ptr`

RAII means resource acquisition is initialization: an object owns a resource and releases it in its destructor. Examples here include `std::ifstream`, `std::ofstream`, `std::lock_guard`, and `std::unique_ptr`.

`std::unique_ptr<Strategy>` expresses exclusive ownership. When it leaves scope, its destructor deletes the concrete strategy automatically. No manual `delete` is required.

### 13.3 References, pointers, and lifetime

The pipeline stores `OrderBook&`, `Strategy&`, and `IMatchingEngine&`. References say these collaborators must exist for the pipeline's lifetime and cannot be null.

The matching engine also stores `exchange::Order*` in an ID index. Those pointers refer to elements inside `std::list<exchange::Order>`. List node addresses remain stable when other elements are inserted, which makes this indexing approach workable. A pointer becomes invalid when its own order is erased, so the implementation erases the index entry before popping the corresponding list element.

This is an important invariant. Replacing `std::list` with a container that relocates elements, such as a growing `std::vector`, would require redesigning the index.

### 13.4 Ordered and hash maps

`std::map` keeps keys sorted. That makes it suitable for prices because the best bid or ask is available at `begin()`. Map operations are typically $O(\log n)$.

`std::unordered_map` is a hash table with expected $O(1)$ lookup. The project uses it for lookup by order ID and symbol. Its iteration order is not stable, which is why backtest reporting sorts symbols before writing output.

### 13.5 Lambdas and callbacks

The pipeline registers a lambda as the matching engine's report callback:

```cpp
engine_.set_report_callback([this](const ExecutionReport& report) { ... });
```

`[this]` captures the pipeline pointer so the lambda can update strategy state and output files. The callback must not outlive the pipeline; the current construction order ensures the pipeline remains alive while its engine is used.

### 13.6 Atomics and the ring buffer

`UdpFeed` has one producer thread and one consumer thread. Atomics coordinate the ring buffer indices. The producer publishes a new head index with release ordering; the consumer checks it with acquire ordering. This ensures the consumer sees the tick data written before the producer announced the new head position.

Atomics prevent data races on the indices, but they do not make every design automatically correct. The implementation is deliberately minimal and suitable for a single-producer/single-consumer teaching example.

### 13.7 Exceptions and error boundaries

The CLI validates user input before entering runtime modes. Runtime file failures throw `std::runtime_error` and the application entry point catches `std::exception`, writes a readable error, and returns a non-zero exit code.

The backtest parser throws `std::invalid_argument` for invalid data fields and adds the failing row number. This prevents malformed historical data from silently becoming a zero-valued trade.

### 13.8 `std::from_chars` versus `std::stoi`

The main CLI uses `std::from_chars` to parse integer delay and port arguments. Unlike `std::stoi`, it does not throw for ordinary parse failures and lets the code verify that every character was consumed. This is a useful pattern for command-line parsing.

## 14. Tests as Executable Documentation

The tests are small standalone programs registered with CTest. They are intentionally direct: each constructs real project objects and checks observable state or reports.

| Test | Main coverage |
|---|---|
| `strategy_state_test` | Position and working-order changes after trade, partial-fill, filled, and sell-side reports. |
| `orderbook_state_test` | Aggregated bid size, partial fill, cancellation, fill removal, duplicate IDs, and overfill rejection. |
| `matching_engine_test` | Self-trade prevention, owner normalization, market tick fills, partial and full fills, FIFO, and best-price priority. |

When adding a behavior change, first decide which module owns the rule. Add a focused test in the corresponding test executable, then run that target directly before running all of CTest.

## 15. Common Workflows

### 15.1 Compare strategies on one scenario

```bash
./build/hft_demo csv data/scenarios/flat_ticks.csv 0 naive
cp data/runtime/trades.csv /tmp/naive_trades.csv
./build/hft_demo csv data/scenarios/flat_ticks.csv 0 optimized
cp data/runtime/trades.csv /tmp/optimized_trades.csv
diff -u /tmp/naive_trades.csv /tmp/optimized_trades.csv
```

Each run overwrites `data/runtime/`, so copy artifacts before starting the next run.

### 15.2 Generate deterministic scenarios

```bash
python3 tools/gen_ticks.py all --count 100 --seed 42 --output-dir data/scenarios
```

The generator uses a dedicated seeded `random.Random` object. The same scenario name, count, and seed produce the same data. When generating `all`, each scenario receives `seed + offset` to keep scenarios deterministic but distinct.

### 15.3 Inspect a fill lifecycle

1. Run a small scenario in CSV mode.
2. Read `[TICK]` output and `data/runtime/trades.csv`.
3. Run `matching_engine_test` to see concise examples of self-trade prevention, partial fill, full fill, FIFO, and best-price behavior.
4. Trace `MatchingEngine::match` with a debugger if you want to watch `remaining` change order by order.

### 15.4 Reproduce a backtest

```bash
./build/backtest_main \
  data/scenarios/synthetic_ticks.csv \
  data/runtime/orders.csv \
  data/runtime/trades.csv \
  0 0 backtest logs/repro.log
cp logs/repro.log /tmp/repro_first.log
./build/backtest_main \
  data/scenarios/synthetic_ticks.csv \
  data/runtime/orders.csv \
  data/runtime/trades.csv \
  0 0 backtest logs/repro.log
diff -u /tmp/repro_first.log logs/repro.log
```

An empty `diff` output means the two logs are identical.

## 16. Troubleshooting

| Symptom | Likely cause and action |
|---|---|
| `CSV file does not exist` | Use a path relative to the project root or provide an absolute path. |
| `delay must be a non-negative integer` | Use `0`, `10`, and similar whole-number delays. |
| `UDP port must be an integer from 1 to 65535` | Choose a valid unprivileged port such as `9000`. |
| No strategy orders appear | The top of book needs both a positive bid and ask before either strategy quotes. |
| `trades.csv` is empty | No local order crossed the synthetic market liquidity. Try a different scenario or inspect the console output. |
| Backtest says a trade row is invalid | Check the header and use `B` or `S` side values with valid numeric timestamp, price, quantity, and ID fields. |
| UDP simulation receives nothing | Confirm the port, host, and sender file. Also note that the current application does not report a failed UDP bind explicitly. |
| Tests pass but behavior changes in Release | Verify assertions are enabled; `assert` checks may be compiled out with `NDEBUG`. |

## 17. Extension Ideas and Boundaries

Good next steps for this toy project include:

- Add a plotting script that reads runtime CSV files and shows price, quotes, fills, position, and equity.
- Add a documented test framework or richer assertion messages.
- Add explicit rejection reports for invalid orders.
- Make the backtest equity curve update per trade or tick.
- Add clean UDP shutdown and startup error reporting.
- Separate external market depth from local order quantities.
- Add scenario-based integration tests from tick input through CSV output.

Features that should remain out of scope unless the project is intentionally re-positioned include live trading, exchange credentials, production risk controls, real venue protocol support, and claims of HFT-grade latency.

## 18. Learning Checklist

After working through this project, you should be able to answer:

- How does a `Tick` move from a feed to a strategy decision?
- Why are `std::map` and `std::list` useful for price-time matching?
- What is the difference between a `Trade` report and a `PartialFill` report?
- Why must the matching index remove a pointer before erasing a list node?
- How does self-trade prevention change a matching outcome?
- How does the strategy keep position separate from working exposure?
- Why does deterministic output require resetting state and sorting unordered-map keys?
- What ownership relationships are expressed by references, `std::unique_ptr`, and RAII-managed streams?

Use the source alongside this guide. The system is small enough that following one tick through every module is practical, and that is where the most durable understanding comes from.
