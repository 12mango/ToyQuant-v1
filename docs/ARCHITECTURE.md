# Architecture

ToyQuant is a small event-driven market-making simulator. It connects market-data input, a simplified order book, strategy decisions, order matching, execution reports, and CSV output. This page explains the implementation by source file; [User Guide](USER_GUIDE.md) contains the commands, scenarios, and experiment workflow.

## 1. System Map

```mermaid
flowchart LR
    CSV[CsvFeed\nCSV rows] --> T[Tick]
    UDP[UdpFeed\nUDP packets] --> T
    BA[Market data adapter\nBinance or future formats] --> RF[ReplayFeed\ntime merge]
    RF --> MEV[MarketEvent\nTrade or BBO]
    T --> P[Pipeline::process_tick]
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
    TR --> BT[BacktestDriver]
    T --> BT
```

The central path is intentionally short:

```text
input -> Tick -> OrderBook / MatchingEngine -> Strategy -> orders -> reports -> output
```

The source map is:

| Responsibility | Main files | Key types or functions |
|---|---|---|
| Shared data | `src/common/types.h` | `Tick`, `Side`, `ExecType`, `TickCallback` |
| Instrument rules | `src/common/instrument_spec.h` | `InstrumentSpec`, `btc_usdt_spec` |
| Input | `src/market/csv_feed.*`, `udp_feed.*` | `CsvFeed::run`, `UdpFeed::loop` |
| Market replay | `src/market/market_data_adapter.*`, `replay_feed.*` | `IMarketEventReader`, `ReplayFeed::run` |
| Coordination | `src/main.cpp` | `Pipeline::process_tick`, output callbacks |
| Local state | `src/orderbook/orderbook.*` | `TopOfBook`, `OrderBook`, `OrderState` |
| Matching | `src/exchange/matching_engine.*` | `MatchingEngine::match`, `send_order` |
| Strategy | `src/strategy/strategy.h`, `market_maker.h` | `Strategy`, two market makers |
| Analysis | `src/backtest/backtest_driver.*` | `BacktestDriver::run`, `print_report` |
| Runtime logging | `src/utils/logger.*` | `Logger::log`, `Logger::error` |

## 2. Shared Types and Input Boundary

**Files:** `src/common/types.h`, `src/market/csv_feed.*`, `src/market/udp_feed.*`

Every feed produces the same `Tick`, so downstream modules are independent of the transport:

```cpp
struct Tick
{
    uint64_t ts = 0;
    std::string symbol;
    double price = 0.0;
    uint64_t size = 0;
    Side side = Side::Unknown;
};

using TickCallback = std::function<void(const Tick&)>;
```

The legacy CSV and UDP paths retain this contract. Trades+BBO replay uses a richer boundary:

```cpp
using MarketEvent = std::variant<MarketTrade, BboQuote>;
```

`IMarketEventReader` isolates vendor schemas from replay. The Binance readers map aggregate trades
and book ticker rows into these domain events; `ReplayFeed` only performs streaming timestamp
merge. To support another vendor, implement readers for its files and add a factory branch in
`make_market_data_readers` without changing `Pipeline`, the strategy, or matching code.

`InstrumentSpec` is shared by the market-data adapter, strategy, order book, and matching engine.
For BTCUSDT it defines a `0.10` price tick and `1000000` integer quantity units per BTC. The replay
strategy therefore uses `0.001 BTC` base orders, a two-tick base spread, and a `0.1 BTC` inventory
limit. Fee rates are carried in the specification for portfolio accounting, which is not yet part
of the L1 replay.

BBO events update the external top of book and trigger quoting. Trade events carry aggressor side
and drive matching. The external BBO is stored separately from local strategy orders, so replacing
a quote cannot delete a local order at the same price.

`CsvFeed::run` reads rows, converts them into `Tick`, and invokes `cb_(t)`. Malformed rows are
reported and skipped. The callback keeps the feed independent from `Pipeline`.

`UdpFeed` has a different transport but the same output contract. On Linux, `recvmmsg` receives
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

### File: `src/main.cpp`, class `Pipeline`

`main.cpp` wires concrete objects together. `run_csv_mode` creates an `OrderBook`, one polymorphic strategy, a `MatchingEngine`, a `Pipeline`, and a `CsvFeed`. The feed only knows its callback; `Pipeline` owns the domain sequence.

```mermaid
sequenceDiagram
    participant F as Feed
    participant P as Pipeline
    participant B as OrderBook
    participant M as MatchingEngine
    participant S as Strategy
    F->>P: process_tick(tick)
    P->>B: on_tick(tick)
    P->>M: process_market_tick(tick)
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

`orders.csv` is written when the pipeline submits a candidate order, before the matching result is known. `trades.csv` receives only `Trade` reports owned by `MarketMaker`; `Resting`, `Filled`, and `Cancelled` are lifecycle events, not additional executions.

### 3.1 Dependency injection at the coordinator boundary

`Pipeline` does not construct the order book, strategy, matching engine, or output streams itself. They are created by `run_csv_mode` and passed into the constructor:

```cpp
auto output_files = open_output_files(csv_file);
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

This is constructor injection: the `Pipeline` declares what it needs, while the caller chooses the concrete implementations. `IOrderBook` and `IMatchingEngine` make the coordinator depend on behavior rather than on `OrderBook` and `MatchingEngine` details. The strategy is injected through the `Strategy` base class, so the same pipeline can run `NaiveMarketMaker` or `OptimizedMarketMaker`:

```cpp
std::unique_ptr<Strategy> make_strategy(const std::string& strategy_name)
{
    if (strategy_name == "naive")
    {
        return std::make_unique<NaiveMarketMaker>(100, 0.00001);
    }
    return std::make_unique<OptimizedMarketMaker>(100, 0.000003);
}
```

Dependency injection keeps construction at the application boundary and the event sequence in
`Pipeline`. Interfaces and callbacks also give focused tests substitution points without requiring
the complete application.

### 3.2 RAII and lifetime-bound resources

The application uses RAII, or Resource Acquisition Is Initialization, to bind cleanup to object lifetime. In CSV mode, the local objects are created in dependency order and remain alive while the feed invokes the pipeline:

```cpp
auto output_files = open_output_files();
OrderBook order_book;
auto strategy = make_strategy(cfg.strategy_name);
Logger logger(to_abs_path("logs/toy_quant.log"));
MatchingEngine engine(&logger);
Pipeline pipeline(output_files.orders, output_files.trades, order_book, *strategy, engine, logger);
CsvFeed feed(csv_file, [&](const Tick& tick) { pipeline.process_tick(tick, true); }, cfg.delay,
             &logger);
feed.run();
```

When `run_csv_mode` returns, destruction runs in reverse declaration order. The feed, pipeline,
logger, strategy, order book, and output streams clean themselves up through object lifetime. The
same cleanup occurs during stack unwinding if the feed throws.

The locking code applies the same rule to synchronization:

```cpp
TopOfBook OrderBook::top(const std::string& symbol)
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

## 4. OrderBook: Market View and Local Order State

**Files:** `src/orderbook/orderbook.h`, `src/orderbook/orderbook.cpp`

`OrderBook` combines two pieces of state:

1. `bids_qty` and `asks_qty` provide the simplified external top of book.
2. `bids_orders` and `asks_orders` hold local strategy orders and their states.

```cpp
struct SideBook
{
    std::map<double, uint64_t, std::greater<double>> bids_qty;
    std::map<double, uint64_t> asks_qty;
    std::map<double, std::list<OrderNode>, std::greater<double>> bids_orders;
    std::map<double, std::list<OrderNode>> asks_orders;
};
```

The comparator is the key rule: `bids_qty.begin()` is the highest bid, while `asks_qty.begin()` is the lowest ask. A `std::list` keeps queue-node addresses stable when other nodes are inserted, allowing `order_index_` to point at an active `OrderNode`. This is readable ordered price-level storage; production systems often use integer ticks instead of `double` prices.

`on_tick` updates one price level from the incoming tick; `top` reads the first level on both sides. A tick does not rebuild complete depth or delete old external levels. The result is a top-of-book teaching abstraction, not a full exchange book.

```mermaid
stateDiagram-v2
    [*] --> New
    New --> Active: add_order
    Active --> PartialFilled: partial fill
    PartialFilled --> PartialFilled: more partial fill
    Active --> Filled: full fill
    PartialFilled --> Filled: remaining filled
    Active --> Cancelled: cancel_order
    New --> Rejected: invalid input
```

`order_index_` contains active orders only. `state_index_` retains terminal states, so `state_for_order` can distinguish `Filled`, `Cancelled`, and unknown `Rejected`. `std::lock_guard<std::mutex>` protects mutations and queries with RAII: the mutex is released automatically on every return path.

### 4.1 Why `map` and `unordered_map` are both present

`OrderBook` mixes two access patterns:

```cpp
std::map<std::string, SideBook> books_;
std::unordered_map<uint64_t, OrderNode*> order_index_;
std::unordered_map<uint64_t, OrderState> state_index_;
```

The `map` versions are used when the code needs ordered iteration by price or symbol. `books_` is keyed by symbol, and each `SideBook` keeps price levels in sorted order:

```cpp
std::map<double, uint64_t, std::greater<double>> bids_qty;
std::map<double, std::list<OrderNode>, std::greater<double>> bids_orders;
```

This makes `bids_qty.begin()` the best bid and `asks_qty.begin()` the best ask, which is exactly what `top()` wants. The downside is that `std::map` is $O(\log n)$ for lookup and iteration is ordered by key.

`unordered_map` is used for order IDs and states, because the code mostly needs direct lookup and duplicate checks:

```cpp
auto it = order_index_.find(order_id);
if (it == order_index_.end()) return false;
```

and

```cpp
if (state_index_.contains(order.id))
{
    return;
}
```

Here the goal is not ordering but constant-time average lookup: `unordered_map` is $O(1)$ average, which is useful when a fill, cancel, or state transition needs to find one active order by ID. In other words, the project chooses `map` for “sorted market view” and `unordered_map` for “fast order identity lookup.”

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

## 6. Strategy: The Main Decision Module

**Files:** `src/strategy/strategy.h`, `src/strategy/market_maker.h`

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

### 6.3 Strategy comparison

For the same `sample_ticks.csv` run, the two strategies diverge immediately:

| Strategy | Submitted orders | Submitted quantity | Cancel requests | Fill rate | Net position | Current equity | Max drawdown |
|---|---:|---:|---:|---:|---:|---:|---:|
| `naive` | 38 | 3800 | 0 | 0.231579 | -880 | 999.286800 | 0.000717 |
| `optimized` | 24 | 1379 | 9 | 0.31037 | -428 | 999.670190 | 0.000334 |

The table uses an initial capital of `1000.0`. It shows the core difference: `naive` is more aggressive and leaves more working orders behind, while `optimized` submits less, cancels stale quotes, and ends with smaller adverse position and drawdown. That is why the optimized version is the better demonstration of stateful market making in this project.

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
| `fill_rate` | Filled quantity divided by submitted quantity | `main.cpp` execution summary |
| `cancel_rate` | Cancel requests divided by submitted orders | `main.cpp` execution summary |
| `inventory_exposure` | Absolute value of net position | `main.cpp` execution summary |
| `max_drawdown` | Largest decline from an equity peak | `BacktestDriver` |

The metric tests cover zero denominators, positive and negative inventory, empty equity curves, and a known peak-to-trough drawdown. This separation lets future metrics be added without expanding `backtest_driver.h` or duplicating formulas in `main.cpp`.

