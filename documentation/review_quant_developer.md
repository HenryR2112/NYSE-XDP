# Senior Quantitative Developer Code Review

## NYSE-XDP Repository

**Reviewer perspective:** Senior Quantitative Developer evaluating simulation correctness, performance architecture, data pipeline integrity, configuration management, numerical precision, and reproducibility.

**Critical constraint:** The software must NOT change its output. This is a refactor/reorganization review only.

---

## Table of Contents

1. [Per-File Assessment](#1-per-file-assessment)
2. [Data Flow Analysis](#2-data-flow-analysis)
3. [Parameter Management Assessment](#3-parameter-management-assessment)
4. [Numerical and Correctness Concerns](#4-numerical-and-correctness-concerns)
5. [Reproducibility Gaps](#5-reproducibility-gaps)
6. [Recommendations for Refactor/Modularization](#6-recommendations-for-refactormodularization)

---

## 1. Per-File Assessment

### C++ Source Files

#### `src/market_maker_sim.cpp` (~1900 lines)

This is the monolithic core of the simulation. It contains the execution model, per-symbol simulation state, process management, results aggregation, CSV output, and CLI argument parsing -- all within a single anonymous namespace.

**Strengths:**
- The `PerSymbolSim` struct (line 227) cleanly encapsulates per-symbol state including dual order books, dual strategies, RNG, feature trackers, and risk tracking.
- The lock-free fast path for symbol lookup (lines 792-813) with sharded mutexes (64 shards, line 767) is well-designed for the threaded execution mode, using proper acquire/release memory ordering.
- The greedy bin-packing file grouping algorithm (lines 1143-1197) provides balanced load distribution across process groups.
- The hybrid multi-process architecture using `fork()` with `MAP_SHARED|MAP_ANONYMOUS` shared memory (lines 1554-1557) is a pragmatic choice that eliminates inter-process lock contention entirely.

**Concerns:**

1. **Monolithic file structure.** At ~1900 lines, this file mixes at least six distinct responsibilities: execution model configuration (lines 56-97), feature tracker data structures (lines 103-169), per-symbol simulation logic (lines 227-757), global symbol storage management (lines 760-813), XDP message dispatch (lines 820-916), results aggregation and output (lines 918-1068), hybrid process architecture (lines 1107-1741), and CLI parsing (lines 1438-1508). Each of these should be an independent compilation unit.

2. **Global mutable state.** The anonymous namespace contains numerous global variables: `g_exec` (line 99), `g_filter_ticker` (line 39), `g_output_dir` (line 40), `g_online_learning` (line 47), `g_sims_array` (line 763), `g_sims_initialized` (line 764), and multiple `std::atomic` counters (lines 770-774). These globals are implicitly shared across threads in threaded mode, making reasoning about correctness difficult.

3. **Memory leak on non-hybrid paths.** `get_or_create_sim_fast()` (line 808) allocates `PerSymbolSim` with `new` but there is no corresponding `delete` anywhere in the threaded or sequential code paths. In hybrid mode, child processes `_exit(0)` (line 1590), which avoids destructors but relies on OS cleanup. In threaded/sequential mode, these allocations leak.

4. **Dual order book redundancy.** Each `PerSymbolSim` maintains two complete `OrderBook` instances (`order_book_baseline` and `order_book_toxicity`, lines 228-229) that receive identical message streams (e.g., `on_add` at lines 566-567 calls `add_order` on both). Since order book state is identical for both strategies, this doubles memory usage and processing time for book maintenance per symbol.

5. **Stale order cleanup heuristic.** The periodic cleanup in `on_add` (lines 569-582) uses a fixed 60-second maximum order age. This is only triggered on ADD messages, meaning symbols with no new ADDs never clean up stale entries.

6. **Emergency fill vector cleanup.** Lines 458-463 force-mark fills as measured when the vector exceeds 10,000 entries. This means some fills may never have their adverse selection properly measured.

7. **Packet callback duplication.** The packet processing callback is defined independently in three places: hybrid `process_file_group` (lines 1218-1236), threaded path (lines 1758-1784), and sequential path.

8. **Non-atomic counter increment.** At line 894, `g_total_executions++` uses non-atomic increment while other counters use `fetch_add`. In threaded mode, this is a data race (undefined behavior).

#### `src/market_maker.hpp` (181 lines)

**Concerns:**

1. **Hardcoded strategy parameters.** Lines 135-156 embed all strategy parameters as private member constants. Only three have setters. These should be injected via a configuration struct.

2. **Legacy alpha parameters.** Lines 151-153 define `alpha1_`, `alpha2_`, `alpha3_` for the old 3-signal static model. `calculate_toxicity_score()` (line 115) appears to be dead code replaced by the SGD model.

3. **Unused `ToxicityWindow` struct.** Lines 163-171 define a struct initialized in `reset()` but never updated during operation.

4. **C-style arrays in `OnlineToxicityModel`.** Lines 22, 29-30 use raw C arrays rather than `std::array`.

#### `src/market_maker.cpp` (484 lines)

**Strengths:**
- Correct handling of all four position transitions in `on_order_filled()` (lines 238-319).
- The `calculate_expected_pnl()` method (lines 399-406) cleanly implements the manuscript's expected PnL equation.
- The SGD update (lines 454-483) correctly implements binary cross-entropy gradient descent with Welford normalization and weight clipping.

**Concerns:**

1. **Redundant toxicity level iteration.** `calculate_toxicity_adjusted_spread()` (lines 14-55) iterates top 5 levels. `get_average_toxicity()` (lines 75-105) iterates top 3 levels. Both call `get_bids()`/`get_asks()` which acquire locks and return full map copies. A single quote update causes 4 separate lock acquisitions and 4 complete map copies.

2. **Inconsistent level counts.** `calculate_toxicity_adjusted_spread()` uses 5 levels (line 27), `get_average_toxicity()` uses 3 levels (line 83), and `build_feature_vector()` uses 3 levels. The manuscript describes L=3.

3. **Warmup predict discontinuity.** During warmup, `predict()` (lines 428-437) applies raw weights to raw features clamped to [0,1]. Post-warmup (lines 440-451), it uses z-score normalization and sigmoid. The prediction function is discontinuous at the warmup boundary.

#### `src/order_book.hpp` (425 lines)

**Concerns:**

1. **`double` as map key.** Lines 321-322 use `std::map<double, uint32_t>`. While safe in the current pipeline (prices always flow through the same `parse_price()` path), floating-point keys are fragile. An integer-price representation would be more robust.

2. **`update_stats()` iterates all levels.** Lines 390-422 iterate over all bids and all asks on every mutation. Maintaining running sums would reduce this from O(L) to O(1) per operation, significant across billions of calls.

3. **Lock contention pattern.** Every public method acquires the instance mutex independently. The `update_market_data()` path causes ~10 separate lock acquisitions per quote update.

4. **Timestamp using wall clock.** Line 129 uses `std::chrono::system_clock::now()` for order timestamps. In simulation replay, wall-clock time is meaningless.

5. **Toxicity metrics never decay.** `ToxicityMetrics` counters are cumulative with no windowing, contradicting the manuscript's description of rolling windows.

6. **`BookStats` has unused toxicity fields.** Lines 93-100 define eight time-windowed toxicity fields that are never computed.

#### `src/reader.cpp` (775 lines)

**Concern: Code duplication.** Duplicates XDP message parsing logic from `market_maker_sim.cpp`. Both independently parse messages with hard-coded byte offsets.

#### `src/common/mmap_pcap_reader.hpp` (297 lines)

Clean RAII with proper move semantics. `MADV_SEQUENTIAL` hint and `preload()` page-touching are well-designed. Minor concerns: `volatile` in preload (line 273), `reinterpret_cast` of mapped memory structures.

#### `src/common/pcap_reader.hpp` (255 lines)

**Concern:** `reinterpret_cast<const uint16_t*>` on potentially unaligned pointers (lines 57, 64, 95-97). Safe on x86/ARM but technically UB per strict aliasing.

#### `src/common/xdp_utils.hpp` (124 lines)

**Concerns:**
1. **`localtime()` is not thread-safe** (line 53). Use `localtime_r()` instead.
2. **Default price multiplier assumption** (lines 42-45). The simulation always uses the default `1e-6` multiplier without checking symbol-specific price scale codes.

#### `src/visualization.cpp` (447 lines)

`#include`d directly by `visualizer_main.cpp` (line 3 of that file) -- a significant anti-pattern that defeats separate compilation.

#### `src/visualizer_main.cpp` (419 lines) and `src/visualizer_pcap.cpp` (1865 lines)

Both contain a replace order side bug: side is hardcoded to `'B'`, treating all replace orders as bid-side regardless of actual side. `visualizer_main.cpp` also has broken `extern` declarations for `reader.cpp` symbols not in its build target.

### Python Scripts

#### `scripts/parameter_sensitivity.py` (187 lines)

Hardcoded data directory (line 26). Regex parsing of simulation stdout is fragile. No `returncode` check on subprocess results.

#### `scripts/analyze_fills.py` (558 lines)

Clean stdlib-only implementation with HAC Newey-West standard errors and per-feature Spearman correlations. Minor: serial bootstrap, normal CDF approximation for p-values.

#### `scripts/analyze_results.py` (348 lines)

Still computes "Annualized Sharpe" by multiplying intraday Sharpe by sqrt(252), which is meaningless for single-day data. The manuscript correctly omits this.

#### `scripts/test_hypotheses.py` (302 lines)

Correct paired t-tests for H2/H3. Uses normal approximation for bootstrap p-values. Treats 14 process groups as independent observations (acknowledged limitation in manuscript).

#### `scripts/run_full_day.sh` (244 lines)

Legacy sequential approach superseded by hybrid mode. Uses macOS-specific `stat` flags and hardcoded paths. Should be marked deprecated.

### Documentation

#### `documentation/market_maker_manuscript.tex` (987 lines)

Comprehensive and scientifically honest. Discrepancy: describes z-score normalization with Welford's algorithm for toxicity, but `order_book.hpp`'s `ToxicityMetrics::get_toxicity_score()` uses cumulative counters without windowing or normalization.

#### `documentation/hft_architecture_paper.tex` (466 lines)

Contains "Annualized Sharpe Ratio (est.) 32.248" (line 359) which the main manuscript correctly excludes. Reports Sharpe of 2.108 (line 391) vs manuscript's 2.031. Should be reconciled.

#### `documentation/ARCHITECTURE.md` (484 lines)

Outdated. Does not document the simulation, hybrid fork architecture, online SGD model, or shared memory aggregation.

### Build/Config

#### `CMakeLists.txt` (229 lines)

**Critical concerns:**
1. **`-ffast-math` in release builds** (line 219) violates IEEE 754 semantics. Enables non-deterministic floating-point across compiler versions.
2. **SDL2 is hard-required** (lines 125-126) even though only visualizer targets need it.
3. **Global release flags** (lines 215-221) apply `-march=native -flto` to all targets including visualizers.

---

## 2. Data Flow Analysis

```
PCAP File I/O (mmap + MADV_SEQUENTIAL)
  -> Network header parsing (Ethernet/IP/UDP extraction)
  -> XDP PacketHeader parsing + message iteration
  -> process_xdp_message() with symbol routing via sharded locks
  -> Per-symbol: order book update x2, feature tracking, execution detection
  -> Quote engine (rate-limited 10us): adverse measurement, eligibility, toxicity prediction
  -> Fill simulation: latency check, price check, queue position consumption
  -> Results aggregation: per-process via shared memory, cross-process via parent
  -> Output: stdout (regex-parseable), CSV (per-fill, per-symbol)
  -> Analysis: Python scripts parse output via regex -> JSON results
```

Key integrity observations:
- Message ordering preserved within groups but not across groups
- Execute-side determination relies on `order_info` map; orders spanning group boundaries are silently dropped
- CSV output includes both measured and unmeasured fills; downstream must filter
- Output format is regex-coupled to analysis scripts with no schema validation

---

## 3. Parameter Management Assessment

**Critical issue:** Seven CLI help defaults do not match code defaults:
- `--latency-us`: help says 20, code is 5.0
- `--queue-fraction`: help says 0.05, code is 0.005
- `--adverse-multiplier`: help says 0.10, code is 0.03
- `--maker-rebate`: help says 0.002, code is 0.0025
- `--max-position`: help says 5000, code is 50000.0
- `--max-loss`: help says 500, code is 5000.0
- `--quote-interval-us`: help says 50, code is 10

Additional issues: two separate parameter systems for overlapping concepts (execution model vs strategy), no parameter validation, no startup parameter logging.

---

## 4. Numerical and Correctness Concerns

- **`-ffast-math`** enables non-IEEE floating-point. Practical impact is negligible but makes cross-compiler reproducibility impossible.
- **Queue position model**: manuscript says lognormal, code uses normal distribution.
- **Welford's algorithm**: correctly used in both inventory variance and SGD normalization.
- **Sigmoid overflow**: bounded by weight clipping; no practical concern.
- **Price as map key**: safe in current pipeline but fragile for future modifications.

---

## 5. Reproducibility Gaps

1. Non-determinism across execution modes (hybrid vs threaded vs sequential)
2. Process group count affects results (hardware-dependent default)
3. No version/compiler/OS recording in output
4. No data path recording in output
5. RNG seeding is deterministic per-symbol but overall result depends on grouping
6. `-ffast-math` + `-march=native` make results hardware-dependent

---

## 6. Recommendations for Refactor/Modularization

### Priority 1: Correctness and Documentation
- **R1.** Fix CLI help defaults to match code (7 mismatches)
- **R2.** Add startup parameter logging for reproducibility
- **R3.** Document/reconcile lognormal vs normal queue position

### Priority 2: Structural Refactoring
- **R4.** Split `market_maker_sim.cpp` into 6 focused modules
- **R5.** Eliminate dual order books (single shared book per symbol)
- **R6.** Extract shared XDP message parsing functions
- **R7.** Unify the packet callback across execution modes

### Priority 3: Performance
- **R8.** Single lock acquisition for all quote-update data (reduce from ~10 to 1)
- **R9.** Maintain running totals in `update_stats()` (O(L) to O(1))
- **R10.** Cache symbol ticker in `PerSymbolSim`

### Priority 4: Build and Configuration
- **R11.** Replace `-ffast-math` with targeted safe flags
- **R12.** Make SDL2 optional for simulation-only builds
- **R13.** Create unified `SimulationConfig` struct

### Priority 5: Cleanup
- **R14.** Remove dead code (ToxicityWindow, unused BookStats fields, legacy alpha params)
- **R15.** Fix memory leak in non-hybrid paths
- **R16.** Deprecate `run_full_day.sh`
- **R17.** Update ARCHITECTURE.md
- **R18.** Reconcile `hft_architecture_paper.tex` with main manuscript

---

## Summary of Key Findings

| Category | Severity | Count |
|----------|----------|-------|
| Functional bugs (visualizer only) | Low | 2 |
| Documentation mismatches (CLI help vs code) | High | 7 |
| Numerical concerns (`-ffast-math`) | Medium | 1 |
| Performance inefficiencies | Medium | 3 |
| Dead code | Low | 4 |
| Memory management | Low | 1 |
| Reproducibility gaps | Medium | 6 |
| Code duplication | Medium | 3 |
| Structural (monolithic file) | Medium | 1 |

The simulation core is functionally correct for its intended purpose. The most impactful improvements are (1) fixing the CLI help defaults to match code, (2) adding parameter logging for reproducibility, and (3) splitting `market_maker_sim.cpp` into focused modules. None of these require changing the simulation's output.
