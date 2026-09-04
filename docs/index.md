# ToyQuant-v1 Documentation

Welcome to the official documentation for **ToyQuant-v1** — an educational, high-performance C++20 HFT & Market Making Engine Demo.

---

## 📚 Documentation Navigation

Explore the technical design and usage guides through the following sections:

- [ Architecture Overview](ARCHITECTURE.md)  
  *Deep dive into the pipeline architecture, memory pool design, lock-free SPSC queues, OrderBook, and STP risk control.*

- [ Scenarios & Mechanics](SCENARIOS.md)  
  *Detailed explanations of backtesting, live-market simulation, order execution flow, and trading strategies.*

- [ User Guide & API](USER_GUIDE.md)  
  *Step-by-step instructions for building with CMake, running GoogleTest suites, configuring parameters, and running benchmarks.*

---

## ⚡ Key Architecture Highlights

- **Modern C++20**: Utilizing concepts, `std::atomic` memory orders, and RAII.
- **Hot Path Zero-Allocation**: Pre-allocated memory pools ensuring $\mathcal{O}(1)$ predictable latency.
- **Lock-Free Concurrency**: Single-Producer Single-Consumer (SPSC) ring buffers for minimal thread contention.
- **In-Memory OrderBook**: Real-time limit order book matching with Self-Match Prevention (STP).