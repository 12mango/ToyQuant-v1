# ToyQuant Documentation

This documentation is organized into three layers:

1. **Getting Started**: how to build, run, and test the project.
2. **Architecture**: what each module does and why the C++ design is structured this way.
3. **Scenarios**: how to use synthetic market conditions to study behavior and compare strategies.

---

## Documentation Navigation

- [User Guide](USER_GUIDE.md)  
  *Build instructions, CLI usage, testing, runtime artifacts, and basic troubleshooting.*

- [Architecture](ARCHITECTURE.md)  
  *A module-by-module explanation of the data flow, matching logic, and the purpose of each C++ component.*

- [Scenarios](SCENARIOS.md)  
  *Built-in scenario catalog and experiment workflow for understanding market behavior and strategy reactions.*

---

## What This Project Covers

- **C++20 trading loop** for feed, order-book update, strategy decision, and matching.
- **CSV and UDP market feeds** for reproduction and simulation.
- **Order-book state and matching rules** with price-time priority and partial fills.
- **Market-making strategies** such as `naive` and `optimized`.
- **Backtest-oriented reporting** covering execution summaries and inventory exposure.
- **CTest-based validation** for the behaviors that matter most to this project.

This project is intentionally educational and does not aim to reproduce a production exchange stack or a production-grade low-latency HFT system.