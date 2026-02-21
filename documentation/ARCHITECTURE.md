# Architecture Documentation

## System Overview

The NYSE-XDP platform is a C++17 market microstructure research system that processes NYSE XDP Integrated Feed PCAP data to simulate toxicity-screened market-making strategies. The system comprises three executables built from shared core libraries:

| Target | Purpose | Primary source |
|--------|---------|---------------|
| `market_maker_sim` | Parallelized backtesting engine (primary) | `market_maker_sim.cpp`, `per_symbol_sim.cpp` |
| `reader` | CLI XDP message parser | `reader.cpp` |
| `visualizer_pcap` | PCAP-driven order book visualizer | `visualizer_pcap.cpp` |

### Simulation Module Structure

The simulator is decomposed into focused compilation units under `namespace mmsim`:

| File | Responsibility |
|------|---------------|
| `execution_model.hpp` | `ExecutionModelConfig` (HFT parameters) + `SimConfig` (runtime bundle) |
| `feature_trackers.hpp` | Circular buffer trackers: `TradeFlowTracker`, `SpreadTracker`, `MomentumTracker` |
| `sim_types.hpp` | Data types: `VirtualOrder`, `StrategyExecState`, `FillRecord`, `SymbolRiskState` |
| `per_symbol_sim.hpp/cpp` | `PerSymbolSim` struct: shared order book, dual strategies, feature extraction, fill simulation |
| `market_maker_sim.cpp` | Orchestration: `main()`, argument parsing, process management, XDP dispatch, results aggregation |

## High-Level Architecture

```
                     ┌──────────────────────────────────────────────┐
                     │              market_maker_sim                 │
                     │                                              │
                     │  main() → fork() N child processes           │
                     │  Each child:                                 │
                     │    mmap PCAP files                           │
                     │    → parse XDP messages                      │
                     │    → per-symbol simulation (PerSymbolSim)    │
                     │    → dual strategy comparison                │
                     │    → write results to shared memory          │
                     │  Parent: waitpid → aggregate → output        │
                     └──────────────┬───────────────────────────────┘
                                    │
                     ┌──────────────▼───────────────────────────────┐
                     │           Core Libraries                      │
                     ├───────────────────────────────────────────────┤
                     │  PerSymbolSim      (per_symbol_sim.hpp/cpp)   │
                     │  SimConfig         (execution_model.hpp)      │
                     │  FeatureTrackers   (feature_trackers.hpp)     │
                     │  SimTypes          (sim_types.hpp)            │
                     │  OrderBook         (order_book.hpp)           │
                     │  MarketMakerStrategy (market_maker.hpp/cpp)   │
                     │  OnlineToxicityModel (market_maker.hpp/cpp)   │
                     │  MmapPcapReader    (mmap_pcap_reader.hpp)     │
                     │  PcapReader        (pcap_reader.hpp)          │
                     │  SymbolMap         (symbol_map.hpp/cpp)       │
                     │  ThreadPool        (thread_pool.hpp)          │
                     │  XDP types/utils   (xdp_types.hpp/utils.hpp) │
                     └───────────────────────────────────────────────┘
```

## Hybrid Multi-Process Architecture

The simulator uses `fork(2)` with POSIX shared memory for maximum throughput on large datasets. This design eliminates all inter-process lock contention.

### Process Lifecycle

```
Parent process
  │
  ├─ Load symbol mappings (12,249 symbols)
  ├─ Collect and sort PCAP files
  ├─ Group files via greedy bin-packing (balanced by total file size)
  ├─ Allocate shared memory (MAP_SHARED | MAP_ANONYMOUS)
  │     └─ ProcessResults array[N] for inter-process aggregation
  │
  ├─ for each group i in [0, N):
  │     fork()
  │     └─ Child process i:
  │          ├─ mmap each PCAP file (MADV_SEQUENTIAL)
  │          ├─ Parse packets → XDP messages → symbol dispatch
  │          ├─ Per-symbol: update order book, run strategies, record fills
  │          ├─ Write per-group results to shared_memory[i]
  │          ├─ Write CSV files if --output-dir specified
  │          └─ _exit(0)  (avoids destructor overhead)
  │
  └─ Parent:
       ├─ waitpid() for all children
       ├─ Read shared_memory[0..N-1]
       ├─ Aggregate portfolio-level statistics
       └─ Print results to stdout
```

### Why fork() Instead of Threads

- **Zero contention**: Each child has its own address space. No mutexes, atomics, or cache-line bouncing between groups.
- **Fault isolation**: A segfault in one child doesn't affect others.
- **Memory efficiency**: Copy-on-write pages mean forked children share read-only data (symbol maps, configuration).
- **Simple aggregation**: Shared memory provides a clean interface for passing results back to the parent.

### File Grouping

Files are assigned to groups using greedy bin-packing sorted by file size (descending). This balances total bytes processed per group rather than file count, avoiding pathological load imbalance from unequal file sizes.

## Per-Symbol Simulation (`PerSymbolSim`)

Each symbol maintains independent simulation state:

```
PerSymbolSim
  ├── OrderBook (order_book)             # Shared across both strategies
  ├── MarketMakerStrategy (mm_baseline)  # No toxicity screening
  ├── MarketMakerStrategy (mm_toxicity)  # With toxicity screening
  ├── StrategyExecState (baseline/toxicity)
  │     ├── VirtualOrder (bid/ask)       # Simulated resting orders
  │     ├── pending_fills vector         # Fills awaiting adverse measurement
  │     └── completed_fills vector       # Measured fills (preserved for CSV)
  ├── Feature Trackers
  │     ├── TradeFlowTracker             # Circular buffer, 100 trades
  │     ├── SpreadTracker                # Circular buffer, 50 observations
  │     └── MomentumTracker              # Circular buffer, 50 observations
  ├── SymbolRiskState                    # Position limits, loss tracking
  ├── OnlineToxicityModel (optional)     # SGD logistic model per symbol
  └── RNG (deterministic, seeded per symbol)
```

### Message Flow Per Symbol

```
XDP EXECUTE message arrives
  │
  ├─ Update order book (add_order / modify_order / delete_order / execute_order)
  ├─ Check rate limit (10μs quote update interval)
  │
  ├─ For each strategy (baseline, toxicity):
  │     ├─ Measure adverse selection on pending fills past lookforward window
  │     ├─ Build feature vector (8 features from book + temporal trackers)
  │     ├─ Compute toxicity score (SGD logistic model)
  │     ├─ Compute expected PnL
  │     ├─ Quote decision: post/update/cancel virtual orders
  │     └─ Fill simulation: check latency, price, queue position
  │
  └─ Update feature trackers (trade flow, spread, momentum)
```

## Strategy Architecture (`MarketMakerStrategy`)

The strategy class in `market_maker.hpp/cpp` encapsulates:

- **Spread calculation**: Base spread + toxicity adjustment + inventory skew
- **Toxicity screening**: Expected PnL filter (binary quote/no-quote decision)
- **Inventory management**: Quadratic skew function, position limits, loss stops
- **Fill handling**: Weighted-average cost basis P&L tracking
- **Online learning**: Optional SGD weight updates after observed adverse outcomes

### Toxicity Model (`OnlineToxicityModel`)

8-feature logistic regression with online SGD:

```
Input: x = [cancel_ratio, ping_ratio, odd_lot_ratio, precision_ratio,
            resistance_ratio, trade_flow, spread_change, momentum]

During warmup (< 50 fills):
  T = sigmoid(w_0 . clamp(x, 0, 1) + b)

After warmup:
  z = (x - running_mean) / running_stddev   (Welford's algorithm)
  T = sigmoid(w . z + b)

Update (after each fill's adverse outcome observed):
  w <- w - eta_t * (T - y) * z    (binary cross-entropy gradient)
  eta_t = eta_0 / (1 + t/1000)    (decaying learning rate)
  w clipped to [-5, 5]
```

## Order Book (`OrderBook`)

Thread-safe limit order book with per-level toxicity tracking:

- **Price levels**: `std::map<double, uint32_t>` (descending for bids, ascending for asks)
- **Order tracking**: `std::unordered_map<uint64_t, Order>` by order ID
- **Toxicity metrics**: Per-level event counters (adds, cancels, volume, ping/odd-lot/precision/resistance counts)
- **Thread safety**: Single `std::mutex` per book instance; getters return copies

## I/O Layer

### Memory-Mapped PCAP Reader (`MmapPcapReader`)

```
File → mmap(2) → madvise(MADV_SEQUENTIAL) → preload(touch pages) → iterate packets
```

- Zero-copy: packets are pointers into the mmap region
- RAII: proper move semantics, munmap on destruction
- Preload: optional page-touching to pre-fault into memory

### Network Stack Parsing

```
Ethernet (14B) → IP (20B+) → UDP (8B) → XDP Packet Header (16B) → Messages
```

All XDP fields are little-endian. Prices are 32-bit integers / 10,000.

## Analysis Pipeline

```
market_maker_sim (C++)
  │
  ├─ stdout: per-group statistics (regex-parseable)
  ├─ CSV: per-fill records (if --output-dir)
  └─ CSV: per-symbol summaries (if --output-dir)
        │
        ▼
Python scripts (stdlib only)
  │
  ├─ analyze_fills.py → advanced_analysis.json
  │     (Spearman, bootstrap CIs, HAC, per-feature correlations)
  │
  ├─ test_hypotheses.py → hypothesis_test_results.json
  │     (5 formal tests: Sharpe, adverse, variance, cross-sectional, MC)
  │
  └─ parameter_sensitivity.py → parameter_sensitivity.json
        (One-at-a-time grid: phi, chi, mu_latency, kappa)
              │
              ▼
        market_maker_manuscript.tex (LaTeX)
```

## Build System

CMake with four targets. The simulation target (`market_maker_sim`) links only against `libxdp_common` (symbol_map). Visualization targets additionally require SDL2, OpenGL, and ImGui.

## Performance Characteristics

On a 14-core Apple M3 Max with 64GB RAM and NVMe storage:

| Metric | Value |
|--------|-------|
| Full-day processing time | ~217 seconds |
| Message throughput (14 processes) | 70M+ messages/second |
| Single-process throughput | 5-8M messages/second |
| Memory per process | ~2GB peak (100K symbol slots) |
| Dataset size | 74GB, 289M packets, 1.27B messages |
| Scaling efficiency | Near-linear to 8 processes |
