# NYSE XDP Market Making Research Platform

A C++ research platform for parsing NYSE XDP (Exchange Data Protocol) Integrated Feed data from PCAP captures and simulating HFT market making strategies with order flow toxicity detection. Includes real-time order book visualization, a parallelized backtesting engine, and a statistical analysis pipeline.

## Overview

This project provides a complete pipeline from raw network captures to publishable research results:

1. **PCAP Parser** -- Decodes NYSE XDP Integrated Feed binary protocol into structured market events
2. **Order Book Reconstruction** -- Maintains full limit order books with microsecond-precision timestamps
3. **Market Maker Simulation** -- Backtests paired strategies (baseline vs. toxicity-aware) with realistic execution modeling
4. **Toxicity Detection** -- Online SGD logistic model with 8 order-book and temporal features
5. **Visualization** -- ImGui-based real-time order book display with PCAP playback controls
6. **Statistical Analysis** -- Python scripts for Spearman correlation, bootstrap CIs, HAC standard errors, and parameter sensitivity grids

## Building

### Prerequisites

- C++17 compiler (GCC 7+, Clang 5+)
- CMake 3.14+
- libpcap
- SDL2 and OpenGL (for visualization targets only)

### macOS

```bash
brew install libpcap cmake sdl2
```

### Linux (Ubuntu/Debian)

```bash
sudo apt-get install libpcap-dev cmake build-essential libsdl2-dev
```

### Compile

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

This produces four executables:

| Target | Description |
|--------|-------------|
| `reader` | Command-line XDP message parser |
| `market_maker_sim` | Parallelized market making backtest engine |
| `visualizer` | Standalone ImGui order book viewer (sample data) |
| `visualizer_pcap` | PCAP-driven order book visualizer with playback |

To build only the simulator:

```bash
cmake --build build --target market_maker_sim
```

Release builds use `-O3 -march=native -flto -ffast-math -funroll-loops` for maximum throughput.

## Data

### PCAP Files

The simulator consumes raw PCAP network captures of NYSE XDP Integrated Feed multicast traffic. Sample data is available from [Databento PCAP Samples](https://databento.com/pcaps#samples).

Expected directory layout:

```
data/
├── symbol_nyse_parsed.csv                          # Symbol index mapping (12,249 symbols)
├── ny4-xnys-pillar-a-20230822T133000.pcap          # Single segment (~3.6 GB)
└── uncompressed-ny4-xnyx-pillar-a-20230822/        # Full day, 121 segments (~74 GB)
    ├── ny4-xnys-pillar-a-20230822T000000.pcap
    ├── ny4-xnys-pillar-a-20230822T001000.pcap
    └── ...
```

### Symbol Mapping

The symbol file is a CSV mapping XDP symbol indices to tickers:

```
symbol_index,ticker,exchange,price_scale
1,A,NYSE,4
2,AA,NYSE,4
...
```

If no symbol file is provided, the parser displays numeric indices.

## Usage

### XDP Parser (`reader`)

```bash
./build/reader <pcap_file> [verbose] [symbol_file] [-t TICKER] [-m MSG_TYPE]
```

| Argument | Description |
|----------|-------------|
| `pcap_file` | Path to PCAP file (required) |
| `verbose` | `0` = simplified, `1` = detailed (default: 0) |
| `symbol_file` | Path to symbol CSV (default: `data/symbol_nyse_parsed.csv`) |
| `-t TICKER` | Filter to a single ticker (e.g., `-t AAPL`) |
| `-m MSG_TYPE` | Filter by message type (e.g., `-m ADD_ORDER`) |

```bash
# Simplified output for all symbols
./build/reader data/ny4-xnys-pillar-a-20230822T133000.pcap

# Verbose output filtered to AAPL
./build/reader data/ny4-xnys-pillar-a-20230822T133000.pcap 1 data/symbol_nyse_parsed.csv -t AAPL
```

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
  data/symbol_nyse_parsed.csv -t AAPL \
  --latency-us 20 --adverse-multiplier 0.10

# With online SGD learning
./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --online-learning --learning-rate 0.01 --warmup-fills 50 \
  --output-dir documentation/analysis_output
```

### Full Day Script (`run_full_day.sh`)

Convenience wrapper that processes all PCAP segments sequentially and aggregates results:

```bash
./scripts/run_full_day.sh                         # All segments
./scripts/run_full_day.sh --trading-hours-only     # 9:30 AM - 4:00 PM ET only
./scripts/run_full_day.sh -t AAPL                  # Single ticker
./scripts/run_full_day.sh --min-size 100            # Skip segments < 100 MB
./scripts/run_full_day.sh --include-small           # Include all segment sizes
./scripts/run_full_day.sh -d /path/to/pcaps         # Custom PCAP directory
```

### PCAP Visualizer (`visualizer_pcap`)

```bash
./build/visualizer_pcap <pcap_file> -t TICKER [-s symbol_file]
```

Features: real-time bid/ask ladder, playback controls (play/pause/seek), spread and mid-price statistics, level-of-book depth display.

## Architecture

### Parallel Execution Model

The simulator uses a hybrid multi-process architecture for maximum throughput on large datasets:

1. PCAP files are grouped across `N` process groups (default: one per CPU core)
2. Each group is launched via `fork()` for process-level isolation
3. Within each process, PCAP files are read via `mmap(2)` for zero-copy access
4. Per-symbol state (`PerSymbolSim`) is maintained independently -- no cross-process communication
5. Results are aggregated after all children exit

For a full day of NYSE data (~74 GB, 121 PCAP files), this completes in approximately 216 seconds on a modern multi-core machine with 14 process groups.

### Simulation Design

Each symbol runs two independent strategies in parallel:

- **Baseline (`mm_baseline`)** -- Quotes at best bid/ask with inventory skew; no toxicity screening
- **Toxicity-aware (`mm_toxicity`)** -- Same quoting logic, but suppresses quotes when expected PnL (after toxicity penalty) is negative

Both strategies share the same execution model parameters (latency, queue position, fees) so their PnL difference isolates the value of toxicity screening.

### Execution Model

The simulator models realistic HFT execution:

- **Latency**: Configurable mean + jitter (default 5 +/- 1 microseconds)
- **Queue position**: Fraction of visible depth ahead of our order (default 0.5%)
- **Adverse selection**: Measured as midprice change over a lookforward window after each fill
- **Fees**: NYSE Tier 1 maker rebate ($0.0025/share) and taker fee ($0.003/share)
- **Risk limits**: Per-symbol position limits and daily loss kill switches

### Order Flow Toxicity Model

The toxicity detection system uses an 8-feature logistic model trained via online SGD:

**Order-book features** (computed from the `OrderBook::ToxicityMetrics` struct):
| Feature | Description |
|---------|-------------|
| `cancel_ratio` | Fraction of order events that are cancellations |
| `ping_ratio` | Fraction of orders with volume < 10 shares |
| `odd_lot_ratio` | Fraction of orders with non-round-lot volume |
| `precision_ratio` | Fraction of orders priced beyond 2 decimal places |
| `resistance_ratio` | Fraction of orders at psychological price levels (.95, .99, .01, .05) |

**Temporal features** (computed from circular buffer trackers in the simulation):
| Feature | Description |
|---------|-------------|
| `trade_flow_imbalance` | Buy/sell volume imbalance over a rolling window of 100 trades |
| `spread_change_rate` | Rate of spread widening/narrowing over a rolling window |
| `price_momentum` | Short-term midprice momentum over a rolling window |

The model predicts P(adverse fill) via a logistic function of the weighted feature vector. Weights are initialized from domain priors (cancel_ratio dominates at 0.4) and optionally updated via SGD with binary cross-entropy loss, using Welford's algorithm for online z-score normalization.

## Analysis Scripts

All scripts are in `scripts/` and require Python 3.8+ with only standard library modules.

### `analyze_fills.py`

Primary statistical analysis of per-fill simulation output:

```bash
python3 scripts/analyze_fills.py --output-dir documentation/analysis_output
```

Produces:
- Toxicity decile analysis (realized adverse PnL by toxicity score bucket)
- Spearman rank correlation of toxicity vs. adverse selection (overall and per-feature)
- Symbol-level bootstrap confidence intervals (10,000 resamples)
- HAC (Newey-West) standard errors for time-series PnL
- Per-feature Spearman correlations for all 8 toxicity features

Requires simulation to have been run with `--output-dir`.

### `test_hypotheses.py`

Hypothesis testing from the research manuscript:

```bash
python3 scripts/test_hypotheses.py documentation/results.txt
```

Tests:
- Sharpe ratio improvement (bootstrap)
- Cross-sectional proxy: fraction of process groups where toxicity PnL > baseline
- Monte Carlo dominance: binomial test vs. p0=0.9

Outputs results to `documentation/hypothesis_test_results.json`.

### `parameter_sensitivity.py`

Grid search over key execution model parameters:

```bash
python3 scripts/parameter_sensitivity.py --sim-binary build/market_maker_sim
```

Varies four parameters across a grid:
- `phi` (queue_position_fraction): 0.005 -- 0.05
- `chi` (adverse_selection_multiplier): 0.03 -- 0.15
- `mu_latency` (latency_us_mean): 5 -- 50
- `spread_mult` (toxicity_spread_multiplier): 0.5 -- 4.0

Outputs results to `documentation/parameter_sensitivity.json`.

### `analyze_results.py`

Quick summary statistics from piped simulation output:

```bash
./build/market_maker_sim ... 2>&1 | python3 scripts/analyze_results.py
```

## XDP Protocol Support

### Specification

- **XDP Integrated Feed Client Specification v2.3a** (October 25, 2019)
- **XDP Common Client Specification v2.3c**
- Supports NYSE Tape A, B, C, Arca, American, National, and Chicago markets

### Message Types

#### Order Book Messages
| Type | Name | Size | Description |
|------|------|------|-------------|
| 100 | Add Order | 39 B | New visible order added to book |
| 101 | Modify Order | 35 B | Price/volume change with position tracking |
| 102 | Delete Order | 25 B | Order removed from book |
| 104 | Replace Order | 42 B | Cancel/replace operation |

#### Trade Messages
| Type | Name | Size | Description |
|------|------|------|-------------|
| 103 | Execute Order | 32 B | Order execution with price and quantity |
| 110 | Non-Displayed Trade | 32 B | Hidden trade execution |
| 111 | Cross Trade | 40 B | Cross trade with type indicator |
| 112 | Trade Cancel | 32 B | Trade cancellation |
| 113 | Cross Correction | 40 B | Cross trade correction |

#### Market Data Messages
| Type | Name | Size | Description |
|------|------|------|-------------|
| 105 | Imbalance | 60 B | Auction imbalance data |
| 106 | Add Order Refresh | 39 B | Order refresh during recovery |
| 114 | Retail Price Improvement | 17 B | RPI indicator (None/Bid/Offer/Both) |
| 223 | Stock Summary | 40 B | OHLC and volume data |

### Binary Format

- All multi-byte fields are **little-endian**
- Prices are 32-bit integers divided by 10,000 (e.g., 176540000 = $17,654.00)
- Structures use `#pragma pack(push, 1)` to match wire format
- XDP Packet Header: 16 bytes (size, delivery flag, message count, sequence number, timestamp)
- XDP Message Header: 4 bytes (message size, message type)

## Project Structure

```
NYSE-XDP/
├── CMakeLists.txt                    # Build configuration (4 targets)
├── README.md
├── src/
│   ├── reader.cpp                    # CLI XDP message parser
│   ├── order_book.hpp                # Limit order book with toxicity metrics
│   ├── market_maker.hpp              # Strategy classes and OnlineToxicityModel (SGD)
│   ├── market_maker.cpp              # Strategy implementation (spread, inventory, quoting)
│   ├── market_maker_sim.cpp          # Backtest engine with fork() parallelism
│   ├── visualization.cpp             # Standalone ImGui visualizer
│   ├── visualizer_main.cpp           # Visualizer components
│   ├── visualizer_pcap.cpp           # PCAP-driven ImGui visualizer with playback
│   ├── common/
│   │   ├── xdp_types.hpp             # XDP message type enums and struct definitions
│   │   ├── xdp_utils.hpp             # Little-endian readers, price/time formatting
│   │   ├── pcap_reader.hpp           # PCAP Ethernet/IP/UDP header extraction
│   │   ├── mmap_pcap_reader.hpp      # Memory-mapped PCAP reader (zero-copy via mmap)
│   │   ├── thread_pool.hpp           # Work-stealing thread pool with futures
│   │   ├── symbol_map.hpp            # Symbol index -> ticker lookup
│   │   └── symbol_map.cpp
│   └── thirdparty/
│       └── imgui/                    # Dear ImGui (vendored)
├── scripts/
│   ├── run_full_day.sh               # Full-day simulation runner with aggregation
│   ├── analyze_fills.py              # Spearman, bootstrap CIs, HAC, decile analysis
│   ├── analyze_results.py            # Quick summary stats from piped output
│   ├── test_hypotheses.py            # Sharpe improvement, cross-sectional, binomial tests
│   └── parameter_sensitivity.py      # Grid search over execution model parameters
├── documentation/
│   ├── market_maker_manuscript.tex   # Research paper (LaTeX)
│   ├── ARCHITECTURE.md               # System architecture documentation
│   ├── strategy_proposal.pdf         # Strategy proposal document
│   ├── hft_architecture_paper.tex    # HFT architecture paper
│   ├── results.txt                   # Raw simulation output (14 process groups)
│   ├── hypothesis_test_results.json  # Hypothesis test outputs
│   ├── parameter_sensitivity.json    # Sensitivity grid results
│   └── analysis_output/              # Per-fill and per-symbol CSVs (14 groups x 2)
├── data/                             # PCAP captures and symbol files (not in git)
└── data_sheets/                      # NYSE XDP protocol specifications (PDF)
```

## Debugging

### VS Code

The project includes `.vscode/launch.json` and `.vscode/tasks.json` with input prompts for selecting PCAP files and symbol mappings.

### Manual

```bash
# Build with debug symbols
cd build && cmake -DCMAKE_BUILD_TYPE=Debug .. && cmake --build .

# Debug with lldb/gdb
lldb ./build/market_maker_sim -- data/ny4-xnys-pillar-a-20230822T133000.pcap \
  data/symbol_nyse_parsed.csv -t AAPL
```

## License

This project is for educational and research purposes. NYSE XDP protocol specifications are copyright Intercontinental Exchange, Inc.

## References

- [NYSE XDP Integrated Feed Client Specification v2.3a](data_sheets/XDP_Integrated_Feed_Client_Specification_v2.3a.pdf)
- [NYSE XDP Common Client Specification v2.3c](data_sheets/XDP_Common_Client_Specification_v2.3c.pdf)
- [Databento PCAP Samples](https://databento.com/pcaps#samples)
- Avellaneda, M. & Stoikov, S. (2008). High-frequency trading in a limit order book. *Quantitative Finance*, 8(3), 217--224.
