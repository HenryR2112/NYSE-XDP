# NYSE XDP Market Making Research Platform

A high-performance C++17 simulation system that processes 74 GB of real NYSE exchange data to evaluate toxicity-screened market-making strategies. Extends the Avellaneda-Stoikov (2008) optimal control framework with real-time adverse selection screening via an online SGD logistic model using 15 microstructure features, validated with formal statistical testing across three filter variants.

> **Henry Ramstad** | Full paper: [`documentation/market_maker_manuscript.pdf`](documentation/market_maker_manuscript.pdf)

---

## Key Results

**Dataset**: NYSE XDP Integrated Feed (Tape A), August 22, 2023. 74 GB, 1.27 billion messages, ~3,500 securities.

| Metric | Baseline | Toxicity-Aware |
|:-------|---------:|---------------:|
| Total P&L | -$363,134 | **+$1,266** |
| Total Fills | 31,390 | 2,223 |
| P&L per Fill | -$11.57 | +$0.57 |
| Adverse Selection Cost | $5,306 | $435 (91.8% reduction) |
| Inventory Variance | 63,641 | 9,137 (85.6% reduction) |
| Quotes Suppressed | 0 | 68,631 |

**The $363K loss avoided is more informative than the $1,266 profit earned.** Adverse selection dominates spread capture for naive strategies; ~80% of quoting opportunities are toxic.

### Filter Variants

All three toxicity-aware filters achieve positive P&L using the same E[PnL] quoting framework:

| Filter | P&L | Fills | P&L/Fill |
|:-------|----:|------:|---------:|
| Walk-Forward (frozen weights) | +$1,353 | 2,236 | +$0.61 |
| SGD Logistic (online learning) | +$1,266 | 2,223 | +$0.57 |
| EWMA Adaptive Threshold | +$277 | 3,204 | +$0.09 |

Walk-forward outperforms live SGD (+$87), confirming online learning does not improve over static initialization.

### Component Ablation

Each rule tested in isolation with the others disabled:

| Component | P&L | % of Full Gain |
|:----------|----:|---------------:|
| E[PnL] filter only | +$1,259 | 100% |
| Spread widening only | -$312,790 | 13.7% |
| OBI adjustment only | -$364,492 | -0.4% |
| **Full strategy** | **+$1,266** | **100%** |

The binary quote/no-quote decision captures all value.

### Statistical Validation

| Test | Result |
|:-----|:-------|
| HAC standard errors (Newey-West) | t = 5.53, p < 10^-7, 95% CI [$18.76, $39.33] per 5-min bin |
| Symbol-level bootstrap | 86.2% of 1,232 active symbols individually improve |
| Cross-sectional robustness | 14/14 process groups profitable |
| Formal hypothesis tests | 4/5 rejected at alpha = 0.05 |
| Spearman correlation | rho = 0.124 (composite), rho = 0.101 (cancel ratio alone), n = 30,816 |

### Inventory Risk Sensitivity

| gamma | P&L | Fills |
|:------|----:|------:|
| 5 x 10^-8 | -$32,896 | 6,206 |
| 1 x 10^-7 | -$4,490 | 3,194 |
| **2 x 10^-7** | **+$1,266** | **2,223** |

A 4x reduction from optimal converts +$1.3K profit to -$32.9K loss.

### Key Negative Result

Online SGD weight adaptation does not improve the toxicity signal beyond static initialization. Cancel ratio alone (rho = 0.101) accounts for nearly all predictive power of the composite score (rho = 0.124). Three temporal features carry no significant signal at the 250 us timescale.

---

## Building

### Prerequisites

- C++17 compiler (GCC 7+, Clang 5+)
- CMake 3.14+
- libpcap
- SDL2 and OpenGL (optional, visualization targets only)

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

| Target | Description |
|:-------|:------------|
| `market_maker_sim` | Parallelized market making backtest engine (primary) |
| `reader` | Command-line XDP message parser |
| `visualizer_pcap` | PCAP-driven order book visualizer with playback |

```bash
# Build only the simulator
cmake --build build --target market_maker_sim
```

---

## Usage

### Market Maker Simulator

```bash
./build/market_maker_sim <pcap_files...> [options]
```

<details>
<summary><strong>General Options</strong></summary>

| Flag | Description | Default |
|:-----|:------------|:--------|
| `-t TICKER` | Filter to single ticker | all symbols |
| `-s, --symbols FILE` | Symbol mapping CSV | `data/symbol_nyse_parsed.csv` |
| `--output-dir DIR` | Write per-fill and per-symbol CSVs | disabled |
| `--seed N` | Random seed | 42 |

</details>

<details>
<summary><strong>Execution Model</strong></summary>

| Flag | Description | Default |
|:-----|:------------|:--------|
| `--latency-us M` | One-way latency (microseconds) | 5 |
| `--latency-jitter-us J` | Latency jitter (microseconds) | 1 |
| `--queue-fraction F` | Queue position fraction of visible depth | 0.005 |
| `--adverse-lookforward-us U` | Adverse selection measurement window (us) | 250 |
| `--adverse-multiplier M` | Adverse selection penalty multiplier | 0.15 |
| `--maker-rebate R` | Maker rebate per share | 0.0025 |
| `--max-position P` | Max position per symbol (shares) | 50000 |
| `--max-loss L` | Max daily loss per symbol ($) | 5000 |
| `--quote-interval-us Q` | Quote update interval (microseconds) | 10 |
| `--fill-mode M` | Fill mode: `cross` or `match` | cross |

</details>

<details>
<summary><strong>Toxicity Model</strong></summary>

| Flag | Description | Default |
|:-----|:------------|:--------|
| `--filter-type TYPE` | Toxicity filter: `logistic` or `ewma` | logistic |
| `--toxicity-threshold T` | Score threshold for quote suppression | 0.50 |
| `--toxicity-multiplier K` | Toxicity-based spread multiplier | 3.0 |
| `--online-learning` | Enable online SGD weight updates | disabled |
| `--learning-rate R` | SGD base learning rate | 0.05 |
| `--warmup-fills N` | Fills before SGD activates | 2 |
| `--walk-forward` | Walk-forward out-of-sample evaluation | disabled |
| `--ablation MODE` | `spread-only`, `pnl-filter-only`, `obi-only` | full |

</details>

<details>
<summary><strong>Parallelism</strong></summary>

| Flag | Description | Default |
|:-----|:------------|:--------|
| `--threads N` | Number of fork() process groups | auto (all cores) |
| `--files-per-group N` | PCAP files per process group | auto |
| `--no-hybrid` | Use thread pool instead of fork() | hybrid mode |
| `--sequential` | Single-threaded, no parallelism | hybrid mode |

</details>

### Reproducing Manuscript Results

```bash
# 1. SGD logistic (primary)
./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --online-learning --output-dir results/logistic_ev22

# 2. Walk-forward (frozen prior-window weights)
./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --online-learning --walk-forward --output-dir results/wf_ev22

# 3. EWMA adaptive threshold
./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --filter-type ewma --output-dir results/ewma_ev22

# 4-6. Ablation variants
./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --online-learning --ablation spread-only --output-dir results/ablation_spread

./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --online-learning --ablation pnl-filter-only --output-dir results/ablation_pnl

./build/market_maker_sim data/uncompressed-ny4-xnyx-pillar-a-20230822/*.pcap \
  --online-learning --ablation obi-only --output-dir results/ablation_obi

# 7. Generate figures
python3 scripts/generate_figures.py
```

Each run processes ~74 GB in ~217 seconds on 14 cores.

---

## Architecture

### Hybrid Multi-Process Execution

```
Parent process
  |-- Load symbol mappings (12,249 symbols)
  |-- Distribute 121 PCAP files as contiguous time-slices across N groups
  |-- Allocate shared memory (MAP_SHARED | MAP_ANONYMOUS)
  |
  |-- fork() x N child processes
  |     |-- mmap(2) + MADV_SEQUENTIAL each PCAP file (zero-copy)
  |     |-- Parse: Ethernet -> IP -> UDP -> XDP Packet Header -> Messages
  |     |-- Dispatch by msg_type -> per-symbol PerSymbolSim
  |     |-- Write ProcessResults to shared memory
  |     +-- _exit(0)
  |
  +-- waitpid() all children -> aggregate -> output
```

- **Zero contention**: Each child has its own address space. No mutexes between groups.
- **Copy-on-write**: Forked children share read-only data (symbol maps, config).
- **Per-symbol storage**: Pre-allocated 100K-slot pointer array, atomic init flags, 64-shard mutexes for lock-free fast path.
- **Performance**: 70M+ msgs/sec aggregate, ~217 seconds for 74 GB on 14-core Apple M3 Max.

### Per-Symbol Simulation

Each symbol maintains a single shared `OrderBook` with two independent strategies:

```
PerSymbolSim
  |-- OrderBook (std::map price levels + unordered_map<uint64_t, Order>)
  |-- mm_baseline    (no toxicity screening)
  |-- mm_toxicity    (E[PnL] filter + spread widening + OBI)
  |-- Feature Trackers
  |     |-- TradeFlowTracker   (circular buffer, 100 trades)
  |     |-- SpreadTracker      (circular buffer, 50 observations)
  |     +-- MomentumTracker    (circular buffer, 50 observations)
  |-- OnlineToxicityModel      (SGD logistic, 15 features)
  +-- SymbolRiskState           (position limits, loss tracking)
```

### E[PnL] Quoting Filter

```
E[PnL] = P_fill * (spread/2 + rebate - mu_adv * T_hat) - gamma * Q^2
                    ^^^^^^^^   ^^^^^^   ^^^^^^^^^^^^^^^   ^^^^^^^^^^^
                    $0.005     $0.0025  adverse cost      inventory penalty

Quote if E[PnL] > 0; suppress otherwise.
```

Must use **base spread** (not toxicity-adjusted) because fills near the NBBO capture base spread regardless of quoted width.

### Toxicity Model

15-feature online SGD logistic regression predicting P(adverse fill):

| Group | Features | Key Signal |
|:------|:---------|:-----------|
| Order book (5) | cancel ratio, ping ratio, odd lot ratio, precision ratio, resistance ratio | cancel ratio (rho = 0.101) |
| Temporal (3) | trade flow imbalance, spread change rate, price momentum | no significant signal |
| Structural (7) | cancel vol intensity, depth imbalance, top-of-book concentration, level asymmetry, abs trade imbalance, large order ratio, normalized spread | cancel vol intensity (rho = 0.123) |

Initial weights: `w_0 = (0.4, 0.2, 0.15, 0.15, 0.1, 0, ..., 0)`. SGD learns weights near initialization, confirming cancellation features dominate.

### Execution Model Parameters

| Parameter | Value | Description |
|:----------|------:|:------------|
| mu_adverse | 0.015 | Adverse cost per unit toxicity |
| gamma_risk | 2e-7 | Quadratic inventory penalty (suppresses at ~75 shares) |
| latency | 5 us | One-way quote latency (FPGA colocated) |
| queue_fraction | 0.005 | Top 0.5% queue positioning |
| chi | 0.15 | Adverse selection penalty multiplier |
| maker_rebate | $0.0025/share | NYSE Tier 1 maker rebate |
| taker_fee | $0.003/share | Taker fee for unwind crosses |

---

## Data

NYSE XDP Integrated Feed multicast traffic (Tape A), captured August 22, 2023.

```
data/
+-- symbol_nyse_parsed.csv                          # 12,249 symbols
+-- uncompressed-ny4-xnyx-pillar-a-20230822/        # 121 segments, ~74 GB
    +-- ny4-xnys-pillar-a-20230822T000000.pcap
    +-- ...
```

### XDP Protocol

NYSE XDP Integrated Feed Client Specification v2.3a. Five message types for order book reconstruction:

| Type | Name | Size | Description |
|-----:|:-----|-----:|:------------|
| 100 | Add Order | 39 B | New visible order with 64-bit ID, price, volume, side |
| 101 | Modify Order | 35 B | Price/volume change with position tracking |
| 102 | Delete Order | 25 B | Order removed from book |
| 103 | Execute Order | 32 B | Order execution with price and quantity |
| 104 | Replace Order | 42 B | Atomic cancel/replace operation |

All multi-byte fields are little-endian. Prices are 32-bit integers divided by 10,000. Structures use `#pragma pack(push, 1)` to match wire format.

---

## Project Structure

```
NYSE-XDP/
|-- CMakeLists.txt
|-- README.md
|-- src/
|   |-- market_maker_sim.cpp        Orchestration, process mgmt, XDP dispatch
|   |-- per_symbol_sim.hpp/.cpp     Per-symbol simulation engine
|   |-- execution_model.hpp         ExecutionModelConfig + SimConfig
|   |-- feature_trackers.hpp        Circular buffer trackers
|   |-- sim_types.hpp               VirtualOrder, FillRecord, SymbolRiskState
|   |-- market_maker.hpp/.cpp       Strategy classes, OnlineToxicityModel
|   |-- order_book.hpp              Limit order book with toxicity metrics
|   |-- reader.cpp                  CLI XDP message parser
|   |-- visualizer_pcap.cpp         PCAP-driven ImGui visualizer
|   +-- common/
|       |-- xdp_types.hpp           XDP message structs (packed, little-endian)
|       |-- xdp_utils.hpp           Price/time formatting utilities
|       |-- pcap_reader.hpp         Network header extraction
|       |-- mmap_pcap_reader.hpp    Memory-mapped PCAP reader (zero-copy)
|       |-- thread_pool.hpp         Work-stealing thread pool
|       |-- symbol_map.hpp/.cpp     Symbol index -> ticker lookup
|       +-- thirdparty/imgui/       Dear ImGui (vendored)
|-- scripts/
|   |-- generate_figures.py         Publication figures (matplotlib/seaborn)
|   |-- test_hypotheses.py          Formal hypothesis tests
|   +-- parameter_sensitivity.py    OAT sensitivity grid
|-- results/                        Simulation output CSVs (gitignored)
|-- documentation/
|   |-- market_maker_manuscript.tex Research paper
|   |-- market_maker_manuscript.pdf Compiled manuscript
|   |-- images/                     Generated figures
|   +-- results_manifest.json       Result provenance and key claims
|-- data/                           PCAP files and symbol maps (gitignored)
+-- data_sheets/                    NYSE XDP protocol specifications
```

---

## Analysis Pipeline

```
market_maker_sim (C++)
  |-- results/<variant>/fills_group_*.csv     per-fill records with cumulative PnL
  +-- results/<variant>/symbols_group_*.csv   per-symbol summaries
        |
        v
scripts/generate_figures.py (matplotlib/seaborn)
  |-- documentation/images/cumulative_pnl.png
  |-- documentation/images/gamma_sensitivity.png
  |-- documentation/images/ablation.png
  +-- documentation/images/symbol_improvement.png
        |
        v
documentation/market_maker_manuscript.tex (LaTeX -> PDF)
```

---

## References

1. Avellaneda, M. & Stoikov, S. (2008). High-frequency trading in a limit order book. *Quantitative Finance*, 8(3), 217-224.
2. Easley, D., Lopez de Prado, M. & O'Hara, M. (2012). Flow toxicity and liquidity in a high-frequency world. *Review of Financial Studies*, 25(5), 1457-1493.
3. Glosten, L. R. & Milgrom, P. R. (1985). Bid, ask and transaction prices in a specialist market with heterogeneously informed traders. *Journal of Financial Economics*, 14(1), 71-100.
4. Kyle, A. S. (1985). Continuous auctions and insider trading. *Econometrica*, 53(6), 1315-1335.
5. Menkveld, A. J. (2013). High frequency trading and the new market makers. *Journal of Financial Markets*, 16(4), 712-740.

---

*This project is for educational and research purposes. NYSE XDP protocol specifications are copyright Intercontinental Exchange, Inc.*
