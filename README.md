# ToyQuant-v1

A reproducible C++ market-making simulator that demonstrates the complete flow from market data through order books, strategy decisions, matching, execution reports, and backtest metrics.

It is a toy project for learning and experimentation, not a production trading system.

## Project Status

This is an educational project under active development. APIs, scenarios, and strategy behavior may change between versions.

## Scope

- Read market data from CSV or receive it over UDP.
- Maintain a simplified top of book and local order state.
- Provide `naive` and `optimized` market-making strategies.
- Match limit orders using price-time priority.
- Produce order, trade, and backtest log files.
- Cover strategy state, order-book state, and matching behavior with CTest.

The project does not include real exchange connectivity, FIX, a risk gateway, persistence and recovery, nanosecond performance guarantees, or full L2 market reconstruction.

## Quick Start

Requirements: CMake 3.16+, a C++20-capable compiler, and Python 3 for generating scenarios.

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the optimized market-making strategy on a CSV scenario:

```bash
./build/toy_quant csv data/scenarios/flat_ticks.csv 0 optimized
```

Run in UDP mode:

```bash
./build/toy_quant udp 9000 naive
python3 tools/udp_sender.py --port 9000
```

Run a backtest:

```bash
./build/backtest_main \
  data/scenarios/synthetic_ticks.csv \
  data/runtime/orders.csv \
  data/runtime/trades.csv \
  0 0 backtest logs/backtest.log
```

## Output

After running `toy_quant`, generated order and trade data is written to:

- `data/runtime/orders.csv`
- `data/runtime/trades.csv`

The default backtest log is `logs/backtest.log`. Repeated runs with the same input, parameters, and output paths should produce identical backtest logs.

## Documentation

- [User Guide](docs/USER_GUIDE.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Market Scenarios](docs/SCENARIOS.md)
- [Data Files](data/README.md)

## Contributing

Bug reports, documentation improvements, and focused tests are welcome. Please open an issue before proposing new features.

## Maintainer

Created and maintained by [12mango](https://github.com/12mango). Contact: [1498159938@qq.com](mailto:1498159938@qq.com).

## Disclaimer

This project is for educational purposes only. It does not provide investment advice and must not be used for live trading.

## License

This project is licensed under the [MIT License](LICENSE).
