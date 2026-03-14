# NYSE XDP Market Making Research Platform

A high-performance C++17 simulation system that processes 74 GB of real NYSE exchange data to evaluate toxicity-screened market-making strategies. Extends the Avellaneda-Stoikov optimal control framework with real-time adverse selection screening via an online SGD logistic model using 15 microstructure features, validated with formal statistical testing across three filter variants.

## Key Results (August 22, 2023 -- Full Trading Day, EV22 Configuration)

| Metric | Baseline | Toxicity-Aware (SGD) |
|--------|----------|----------------------|
| Total P&L | -$363,134 | +$1,266 |
| Total Fills | 31,390 | 2,223 |
| P&L per Fill | -$11.57 | +$0.57 |
| Adverse Selection Cost | $5,306 | $435 (91.8% reduction) |
| Inventory Variance | 63,641 | 9,137 (85.6% reduction) |
| Quotes Suppressed | 0 | 68,631 |
| Unwind Crosses | 4,639 ($1,599) | 598 ($196) |

**The $363K loss avoided is more informative than the $1,266 profit earned**: it establishes that adverse selection dominates spread capture for naive strategies, and that ~80% of quoting opportunities are toxic.

### Three Filter Variants -- All Profitable

| Filter | Total P&L | Fills | P&L/Fill |
|--------|-----------|-------|----------|
| SGD Logistic (online) | +$1,266 | 2,223 | +$0.57 |
| Walk-Forward (frozen weights) | +$1,353 | 2,236 | +$0.61 |
| EWMA Adaptive Threshold | +$277 | 3,204 | +$0.09 |

Walk-forward outperforms live SGD, confirming that online learning does not improve over static initialization.

### Statistical Validation

- **HAC standard errors** (Newey-West): t = 5.53, p < 10^-7, 95% CI for 5-min PnL [$18.76, $39.33]
- **Symbol bootstrap** (1,232 active symbols): 86.2% individually improve over baseline
- **Cross-sectional**: 14/14 process groups individually profitable
- **Hypothesis tests**: 4/5 rejected at alpha = 0.05 (H5 underpowered at n=14)
- **Spearman correlation**: rho = 0.124 (composite), cancel ratio alone rho = 0.101 (n = 30,816)

### Component Ablation

| Component | P&L | % of Full Gain |
|-----------|-----|----------------|
| E[PnL] filter only | +$1,259 | 100% |
| Spread widening only | -$312,790 | 13.7% |
| OBI adjustment only | -$364,492 | -0.4% |
| Full strategy | +$1,266 | 100% |

The binary quote/no-quote decision captures all value. Spread widening and OBI are secondary.

### Key Negative Result

Online SGD weight adaptation does not improve the toxicity signal beyond static initialization. Cancel ratio alone (rho = 0.101) accounts for nearly all predictive power of the composite score (rho = 0.124). Three temporal features carry no significant signal at the 250us timescale.

### Critical Parameter

The inventory risk penalty gamma is the sole critical parameter:

| gamma | P&L | Fills |
|-------|-----|-------|
| 5e-8 | -$32,896 | 6,206 |
| 1e-7 | -$4,490 | 3,194 |
| **2e-7** | **+$1,266** | **2,223** |

A 4x reduction from optimal converts +$1.3K profit to -$32.9K loss.

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
| `--adverse-multiplier M` | Adverse selection penalty multiplier | 0.15 |
| `--maker-rebate R` | Maker rebate per share | 0.0025 |
| `--max-position P` | Max position per symbol (shares) | 50000 |
| `--max-loss L` | Max daily loss per symbol ($) | 5000 |
| `--quote-interval-us Q` | Quote update interval (microseconds) | 10 |
| `--fill-mode M` | Fill mode: `cross` or `match` | cross |

**Toxicity Model:**

| Flag | Description | Default |
|------|-------------|---------|
| `--filter-type TYPE` | Toxicity filter: `logistic` or `ewma` | logistic |
| `--toxicity-threshold T` | Toxicity score threshold for quote suppression | 0.50 |
| `--toxicity-multiplier K` | Toxicity-based spread multiplier | 3.0 |
| `--online-learning` | Enable online SGD weight updates | disabled |
| `--learning-rate R` | SGD base learning rate | 0.05 |
| `--warmup-fills N` | Fills before SGD activates | 2 |
| `--walk-forward` | Enable walk-forward out-of-sample evaluation | disabled |
| `--ablation MODE` | Ablation mode: `spread`, `pnl`, `obi` | full |

**Parallelism:**

| Flag | Description | Default |
|------|-------------|---------|
| `--threads N` | Number of fork() process groups | auto (all cores) |
| `--files-per-group N` | PCAP files per process group | auto |
| `--no-hybrid` | Use thread pool instead of fork() | hybrid mode |
| `--sequential` | Single-threaded, no parallelism | hybrid mode |

**Examples:**

```bash
# EV22 configuration — SGD logistic (produces results matching manuscript)
./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --online-learning --output-dir results/logistic_ev22

# Walk-forward variant (frozen weights, out-of-sample)
./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --online-learning --walk-forward --output-dir results/wf_ev22

# EWMA adaptive threshold filter
./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --filter-type ewma --output-dir results/ewma_ev22

# Component ablation (E[PnL] filter only)
./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --online-learning --ablation pnl --output-dir results/ablation_pnl

# Single ticker with custom parameters
./build/market_maker_sim data/ny4-xnys-pillar-a-20230822T133000.pcap \
  -t AAPL --latency-us 20 --adverse-multiplier 0.10
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

```
market_maker_sim (C++)
  ├── results/<variant>/fills_group_*.csv    (per-fill records with cumulative PnL)
  └── results/<variant>/symbols_group_*.csv  (per-symbol summaries)
        │
        ▼
scripts/generate_figures.py (matplotlib/seaborn)
  ├── documentation/images/cumulative_pnl.png
  ├── documentation/images/gamma_sensitivity.png
  ├── documentation/images/ablation.png
  └── documentation/images/symbol_improvement.png
        │
        ▼
documentation/market_maker_manuscript.tex (LaTeX)
```

### `generate_figures.py` -- Publication Figures

```bash
python3 scripts/generate_figures.py
```

Produces 4 PNG figures from the `results/` directory (requires matplotlib, seaborn, numpy, pandas).

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

1. PCAP files are distributed as contiguous time-slices across N process groups
2. Each group runs in an isolated child process (zero lock contention between groups)
3. Within each process, files are read via `mmap(2)` with `MADV_SEQUENTIAL` for zero-copy access
4. Per-symbol state (`PerSymbolSim`) is maintained via pre-allocated 100K-slot arrays with atomic init flags and 64-shard mutexes
5. Results aggregate via `ProcessResults` structs in shared memory after all children exit via `waitpid`

Full day (~74 GB, 121 PCAP files) completes in ~217 seconds on 14 cores at 70M+ messages/second.

### Simulation Design

Each symbol runs two independent strategies on a single shared `OrderBook`:

- **Baseline (`mm_baseline`)** -- Quotes at best bid/ask with inventory skew; no toxicity screening
- **Toxicity-aware (`mm_toxicity`)** -- Same quoting logic, but suppresses quotes when E[PnL] < 0

Both share identical execution model parameters so their PnL difference isolates the value of toxicity screening.

The E[PnL] filter computes: `P_fill * (spread/2 + rebate - mu_adv * T_hat) - gamma * Q^2`

The E[PnL] must use the **base spread** (not toxicity-adjusted spread) because fills near the NBBO capture base spread regardless of quoted width.

### Toxicity Model (15-Feature SGD Logistic)

**Order-book features** (5, from `OrderBook::ToxicityMetrics`):

| Feature | Weight (init) | Description |
|---------|--------------|-------------|
| `cancel_ratio` | 0.40 | Fraction of events that are cancellations |
| `ping_ratio` | 0.20 | Fraction of orders with volume < 10 shares |
| `odd_lot_ratio` | 0.15 | Fraction with non-round-lot volume |
| `precision_ratio` | 0.15 | Fraction priced beyond 2 decimal places |
| `resistance_ratio` | 0.10 | Fraction at psychological price levels |

**Temporal features** (3, from per-symbol circular buffers):

| Feature | Weight (init) | Description |
|---------|--------------|-------------|
| `trade_flow_imbalance` | 0.00 | Buy/sell volume imbalance (100 trades) |
| `spread_change_rate` | 0.00 | Rate of spread change (50 observations) |
| `price_momentum` | 0.00 | Short-term midprice momentum (50 observations) |

**Structural features** (7, from top-L level statistics):

| Feature | Weight (init) | Description |
|---------|--------------|-------------|
| `cancel_vol_intensity` | 0.00 | Volume cancelled / volume added |
| `top_of_book_conc` | 0.00 | Best-level qty fraction of total depth |
| `depth_imbalance` | 0.00 | Bid/ask depth ratio |
| `level_asymmetry` | 0.00 | Bid/ask level count ratio |
| `abs_trade_imbalance` | 0.00 | Absolute trade flow imbalance |
| `large_order_ratio` | 0.00 | Orders > 200 shares / total events |
| `normalized_spread` | 0.00 | Spread / mid-price |

Predicts P(adverse fill) via sigmoid. Weights optionally updated via SGD with binary cross-entropy loss and Welford z-score normalization.

### Execution Model

| Parameter | Value | Description |
|-----------|-------|-------------|
| mu_adverse | 0.015 | Adverse cost per unit toxicity |
| gamma_risk | 2e-7 | Quadratic inventory penalty (suppresses at ~75 shares) |
| latency | 5 us | One-way quote latency (FPGA colocated) |
| queue_fraction | 0.005 | Top 0.5% queue positioning |
| chi | 0.15 | Adverse selection penalty multiplier |
| maker_rebate | $0.0025/share | NYSE Tier 1 maker rebate |
| taker_fee | $0.003/share | Taker fee for unwind crosses |

## Data

PCAP files of NYSE XDP Integrated Feed multicast traffic captured August 22, 2023.

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
├── CMakeLists.txt                    # Build configuration
├── README.md
├── src/
│   ├── market_maker_sim.cpp          # Orchestration: main(), process mgmt, XDP dispatch
│   ├── per_symbol_sim.hpp / .cpp     # Per-symbol simulation (PerSymbolSim)
│   ├── execution_model.hpp           # ExecutionModelConfig + SimConfig structs
│   ├── feature_trackers.hpp          # Circular buffer trackers (trade flow, spread, momentum)
│   ├── sim_types.hpp                 # VirtualOrder, FillRecord, SymbolRiskState
│   ├── market_maker.hpp / .cpp       # Strategy classes and OnlineToxicityModel (SGD)
│   ├── order_book.hpp                # Limit order book with toxicity metrics
│   ├── reader.cpp                    # CLI XDP message parser
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
│   ├── generate_figures.py           # Publication figures (matplotlib/seaborn)
│   ├── test_hypotheses.py            # Hypothesis tests (Sharpe, cross-sectional, binomial)
│   └── parameter_sensitivity.py      # Sensitivity grid search
├── results/                          # Simulation output CSVs (gitignored)
│   ├── logistic_ev22/                # SGD logistic filter results
│   ├── wf_ev22/                      # Walk-forward filter results
│   ├── ewma_ev22/                    # EWMA filter results
│   ├── ablation_spread/              # Spread-only ablation
│   ├── ablation_pnl/                 # PnL-filter-only ablation
│   └── ablation_obi/                 # OBI-only ablation
├── documentation/
│   ├── market_maker_manuscript.tex   # Research paper (LaTeX)
│   ├── images/                       # Publication figures (generated)
│   ├── hypothesis_test_results.json  # Hypothesis test outputs
│   └── parameter_sensitivity.json    # Sensitivity grid results
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
- Glosten, L. R. & Milgrom, P. R. (1985). Bid, ask and transaction prices in a specialist market with heterogeneously informed traders. *Journal of Financial Economics*, 14(1), 71--100.
