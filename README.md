# NYSE XDP Market Making Research Platform

A high-performance C++17 simulation system that processes 74GB of real NYSE exchange data to evaluate toxicity-screened market-making strategies. Extends the Avellaneda-Stoikov optimal control framework with real-time adverse selection screening via an online SGD logistic model, validated with formal statistical testing.

## Key Results (August 22, 2023 -- Full Trading Day)

| Metric | Baseline | Toxicity-Aware |
|--------|----------|----------------|
| Total P&L | -$16,342 | $792 |
| Intraday Sharpe | -1.454 | 2.031 |
| Fills | 21,233 | 4,671 |
| Adverse Selection | $339.63 | $95.56 (72% reduction) |
| Inventory Variance | 63,641 | 9,137 (86% reduction) |

**The $16,342 loss avoided is more informative than the $792 profit earned**: it establishes that adverse selection dominates spread capture for naive strategies, and that ~80% of quoting opportunities are toxic.

### Statistical Validation

- **Symbol bootstrap** (3,731 symbols): 95% CI [$747, $837], 47.2% of individual symbols improve (portfolio diversification effect)
- **HAC standard errors** (Newey-West): t = 5.53, p < 10^-7, SE inflation 1.51x
- **Cross-sectional**: 14/14 process groups individually profitable
- **Hypothesis tests**: 4/5 rejected at alpha = 0.05 (H5 underpowered at n=14)

### Key Negative Result

Online SGD weight adaptation does not improve the toxicity signal beyond static initialization. Per-feature analysis reveals that **cancellation rate alone** (rho = 0.116) accounts for nearly all predictive power of the composite score (rho = 0.118). Three temporal features (trade flow imbalance, spread dynamics, price momentum) carry no significant signal at the 250us timescale.

## Building

### Prerequisites

- C++17 compiler (GCC 7+, Clang 5+)
- CMake 3.14+
- libpcap
- SDL2 and OpenGL (optional, for visualization targets only)

```bash
# macOS
brew install libpcap cmake sdl2

# Linux (Ubuntu/Debian)
sudo apt-get install libpcap-dev cmake build-essential libsdl2-dev
```

### Compile

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

Build targets:

| Target | Description |
|--------|-------------|
| `market_maker_sim` | Parallelized market making backtest engine (primary) |
| `reader` | Command-line XDP message parser |
| `visualizer` | Standalone ImGui order book viewer |
| `visualizer_pcap` | PCAP-driven order book visualizer with playback |

To build only the simulator:

```bash
cmake --build build --target market_maker_sim
```

## Usage

### Market Maker Simulator (`market_maker_sim`)

```bash
./build/market_maker_sim <pcap_files...> [options]
```

**General Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `-t TICKER` | Filter to single ticker | all symbols |
| `-s, --symbols FILE` | Symbol mapping CSV | `data/symbol_nyse_parsed.csv` |
| `--output-dir DIR` | Write per-fill and per-symbol CSVs | disabled |
| `--seed N` | Random seed | 42 |

**Execution Model:**

| Flag | Description | Default |
|------|-------------|---------|
| `--latency-us M` | One-way latency (microseconds) | 5 |
| `--latency-jitter-us J` | Latency jitter (microseconds) | 1 |
| `--queue-fraction F` | Queue position as fraction of visible depth | 0.005 |
| `--adverse-lookforward-us U` | Adverse selection measurement window (microseconds) | 250 |
| `--adverse-multiplier M` | Adverse selection penalty multiplier | 0.03 |
| `--maker-rebate R` | Maker rebate per share | 0.0025 |
| `--max-position P` | Max position per symbol (shares) | 50000 |
| `--max-loss L` | Max daily loss per symbol ($) | 5000 |
| `--quote-interval-us Q` | Quote update interval (microseconds) | 10 |
| `--fill-mode M` | Fill mode: `cross` or `match` | cross |

**Toxicity Model:**

| Flag | Description | Default |
|------|-------------|---------|
| `--toxicity-threshold T` | Toxicity score threshold for quote suppression | 0.75 |
| `--toxicity-multiplier K` | Toxicity-based spread multiplier | 1.0 |
| `--online-learning` | Enable online SGD weight updates | disabled |
| `--learning-rate R` | SGD base learning rate | 0.01 |
| `--warmup-fills N` | Fills before SGD activates | 50 |

**Parallelism:**

| Flag | Description | Default |
|------|-------------|---------|
| `--threads N` | Number of fork() process groups | auto (all cores) |
| `--files-per-group N` | PCAP files per process group | auto |
| `--no-hybrid` | Use thread pool instead of fork() | hybrid mode |
| `--sequential` | Single-threaded, no parallelism | hybrid mode |

**Examples:**

```bash
# Full day simulation with CSV output (14 process groups, ~216 seconds)
./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --output-dir documentation/analysis_output

# Single ticker with custom parameters
./build/market_maker_sim data/ny4-xnys-pillar-a-20230822T133000.pcap \
  -t AAPL --latency-us 20 --adverse-multiplier 0.10

# With online SGD learning
./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --online-learning --learning-rate 0.01 --warmup-fills 50 \
  --output-dir documentation/analysis_output
```

### XDP Parser (`reader`)

```bash
./build/reader <pcap_file> [verbose] [symbol_file] [-t TICKER] [-m MSG_TYPE]
```

### PCAP Visualizer (`visualizer_pcap`)

```bash
./build/visualizer_pcap <pcap_file> -t TICKER [-s symbol_file]
```

Real-time bid/ask ladder with playback controls, spread/mid-price statistics, and depth display.

## Analysis Pipeline

All scripts are in `scripts/` and require Python 3.8+ with standard library only (no numpy/pandas/scipy).

### `analyze_fills.py` -- Primary Statistical Analysis

```bash
python3 scripts/analyze_fills.py --output-dir documentation/analysis_output
```

Produces: toxicity decile analysis, Spearman rank correlations (overall + per-feature), symbol-level bootstrap CIs (10,000 resamples), and HAC (Newey-West) standard errors.

### `test_hypotheses.py` -- Formal Hypothesis Testing

```bash
python3 scripts/test_hypotheses.py documentation/results.txt
```

Tests five hypotheses: Sharpe improvement (bootstrap), adverse selection reduction (paired t), inventory variance reduction (paired t), cross-sectional robustness (binomial), and Monte Carlo dominance (binomial).

### `parameter_sensitivity.py` -- Sensitivity Grid

```bash
python3 scripts/parameter_sensitivity.py --sim-binary build/market_maker_sim
```

Varies four parameters: queue fraction (phi), adverse multiplier (chi), latency (mu), and spread multiplier (kappa).

## Architecture

### Hybrid Multi-Process Execution

The simulator uses `fork(2)` with `MAP_SHARED|MAP_ANONYMOUS` shared memory for parallelism:

1. PCAP files are load-balanced across N process groups via greedy bin-packing
2. Each group runs in an isolated child process (zero lock contention between groups)
3. Within each process, files are read via `mmap(2)` with `MADV_SEQUENTIAL` for zero-copy access
4. Per-symbol state (`PerSymbolSim`) is maintained independently per process
5. Results aggregate via shared memory after all children exit via `waitpid`

Full day (~74GB, 121 PCAP files) completes in ~216 seconds on 14 cores at 70M+ messages/second.

### Simulation Design

Each symbol runs two independent strategies in parallel:

- **Baseline (`mm_baseline`)** -- Quotes at best bid/ask with inventory skew; no toxicity screening
- **Toxicity-aware (`mm_toxicity`)** -- Same quoting logic, but suppresses quotes when expected PnL (after toxicity penalty) is negative

Both share identical execution model parameters so their PnL difference isolates the value of toxicity screening.

### Toxicity Model (8-Feature SGD Logistic)

**Order-book features** (from `OrderBook::ToxicityMetrics`):

| Feature | Weight (init) | Description |
|---------|--------------|-------------|
| `cancel_ratio` | 0.40 | Fraction of events that are cancellations |
| `ping_ratio` | 0.20 | Fraction of orders with volume < 10 shares |
| `odd_lot_ratio` | 0.15 | Fraction with non-round-lot volume |
| `precision_ratio` | 0.15 | Fraction priced beyond 2 decimal places |
| `resistance_ratio` | 0.10 | Fraction at psychological price levels |

**Temporal features** (from per-symbol circular buffers):

| Feature | Weight (init) | Description |
|---------|--------------|-------------|
| `trade_flow_imbalance` | 0.00 | Buy/sell volume imbalance (100 trades) |
| `spread_change_rate` | 0.00 | Rate of spread change (50 observations) |
| `price_momentum` | 0.00 | Short-term midprice momentum (50 observations) |

Predicts P(adverse fill) via sigmoid. Weights optionally updated via SGD with binary cross-entropy loss and Welford z-score normalization.

## Data

PCAP files of NYSE XDP Integrated Feed multicast traffic. Sample data available from [Databento PCAP Samples](https://databento.com/pcaps#samples).

```
data/
├── symbol_nyse_parsed.csv                          # Symbol index mapping (12,249 symbols)
└── uncompressed-ny4-xnyx-pillar-a-20230822/        # Full day, 121 segments (~74 GB)
    ├── ny4-xnys-pillar-a-20230822T000000.pcap
    └── ...
```

## XDP Protocol Support

- **XDP Integrated Feed Client Specification v2.3a** (October 25, 2019)
- **XDP Common Client Specification v2.3c**

### Message Types

| Type | Name | Size | Description |
|------|------|------|-------------|
| 100 | Add Order | 39 B | New visible order added to book |
| 101 | Modify Order | 35 B | Price/volume change with position tracking |
| 102 | Delete Order | 25 B | Order removed from book |
| 103 | Execute Order | 32 B | Order execution with price and quantity |
| 104 | Replace Order | 42 B | Cancel/replace operation |

All multi-byte fields are little-endian. Prices are 32-bit integers divided by 10,000. Structures use `#pragma pack(push, 1)` to match wire format.

## Project Structure

```
NYSE-XDP/
├── CMakeLists.txt                    # Build configuration (4 targets)
├── README.md
├── src/
│   ├── market_maker_sim.cpp          # Orchestration: main(), process mgmt, XDP dispatch
│   ├── per_symbol_sim.hpp / .cpp     # Per-symbol simulation (PerSymbolSim, 500+ lines)
│   ├── execution_model.hpp           # ExecutionModelConfig + SimConfig structs
│   ├── feature_trackers.hpp          # Circular buffer trackers (trade flow, spread, momentum)
│   ├── sim_types.hpp                 # VirtualOrder, FillRecord, SymbolRiskState
│   ├── market_maker.hpp / .cpp       # Strategy classes and OnlineToxicityModel (SGD)
│   ├── order_book.hpp                # Limit order book with toxicity metrics
│   ├── reader.cpp                    # CLI XDP message parser
│   ├── visualization.cpp             # Standalone ImGui order book viewer
│   ├── visualizer_pcap.cpp           # PCAP-driven ImGui visualizer with playback
│   └── common/
│       ├── xdp_types.hpp             # XDP message structs (packed, little-endian)
│       ├── xdp_utils.hpp             # Price/time formatting utilities
│       ├── pcap_reader.hpp           # PCAP Ethernet/IP/UDP header extraction
│       ├── mmap_pcap_reader.hpp      # Memory-mapped PCAP reader (zero-copy)
│       ├── thread_pool.hpp           # Work-stealing thread pool
│       ├── symbol_map.hpp / .cpp     # Symbol index -> ticker lookup
│       └── thirdparty/imgui/         # Dear ImGui (vendored)
├── scripts/
│   ├── reproduce.sh                  # Full reproducible pipeline (build → sim → analysis)
│   ├── analyze_fills.py              # Spearman, bootstrap CIs, HAC, decile analysis
│   ├── test_hypotheses.py            # Hypothesis tests (Sharpe, cross-sectional, binomial)
│   └── parameter_sensitivity.py      # Sensitivity grid search
├── documentation/
│   ├── market_maker_manuscript.tex   # Research paper (LaTeX)
│   ├── ARCHITECTURE.md               # System architecture documentation
│   ├── results.txt                   # Raw simulation output (14 process groups)
│   ├── hypothesis_test_results.json  # Hypothesis test outputs
│   ├── parameter_sensitivity.json    # Sensitivity grid results
│   └── analysis_output/              # Per-fill and per-symbol CSVs
├── data/                             # PCAP captures and symbol files (gitignored)
└── data_sheets/                      # NYSE XDP protocol specifications (PDF)
```

## License

This project is for educational and research purposes. NYSE XDP protocol specifications are copyright Intercontinental Exchange, Inc.

## References

- [NYSE XDP Integrated Feed Client Specification v2.3a](data_sheets/XDP_Integrated_Feed_Client_Specification_v2.3a.pdf)
- [NYSE XDP Common Client Specification v2.3c](data_sheets/XDP_Common_Client_Specification_v2.3c.pdf)
- Avellaneda, M. & Stoikov, S. (2008). High-frequency trading in a limit order book. *Quantitative Finance*, 8(3), 217--224.
- Easley, D., Lopez de Prado, M. & O'Hara, M. (2012). Flow toxicity and liquidity in a high-frequency world. *Review of Financial Studies*, 25(5), 1457--1493.
