# Software Engineering Code Review: NYSE-XDP

**Reviewer role:** Senior Software Engineer
**Date:** February 20, 2026
**Scope:** Full codebase review (excluding `thirdparty/imgui`)
**Constraint:** Refactoring and reorganization only. No changes to simulation output.

---

## Executive Summary

The NYSE-XDP project is a high-performance market microstructure research system that processes 74GB of NYSE XDP Integrated Feed data to simulate toxicity-aware market making. The core simulation (`market_maker_sim.cpp` + `market_maker.hpp/cpp`) is technically strong: the hybrid multi-process architecture, mmap-based I/O, sharded locking, and online SGD toxicity model are well-engineered. The accompanying manuscript and statistical analysis are thorough and honest about limitations.

However, the codebase shows clear signs of organic growth. The primary deliverable (`market_maker_sim.cpp`) is a 1,900-line god-file with everything in an anonymous namespace. The visualization subsystem has serious structural problems: one file `#include`s another `.cpp` file, there are duplicate class definitions, and broken extern declarations. Several files contain dead code, debug artifacts, and hardcoded paths. The documentation is partially outdated, and generated results sit in the same directory tree as source code with no protection against accidental overwrite.

None of these issues affect correctness or output. They affect maintainability, readability, and how the project presents to a reviewer, hiring manager, or academic audience.

---

## Per-File Review

### C++ Source Files

#### `src/market_maker_sim.cpp` (~1,900 lines)

**Purpose:** The entire market maker simulation: configuration, feature trackers, virtual orders, execution state, fill records, risk state, per-symbol simulation, XDP message handling, results aggregation, CSV output, print functions, hybrid multi-process orchestration, and `main()` with argument parsing.

**Issues:**

- **God-file.** Everything lives in a single anonymous namespace (line 37: `namespace {`). This file contains at least 10 distinct responsibilities: configuration structs (`ExecutionModelConfig`), feature tracking (`TradeFlowTracker`, `SpreadTracker`, `MomentumTracker`), virtual order management (`VirtualOrder`), strategy execution state (`StrategyExecState`), fill recording (`FillRecord`), risk management (`SymbolRiskState`), the massive `PerSymbolSim` struct, symbol storage with sharded locks, all five XDP message handlers, results printing, CSV export, hybrid process management (`ProcessResults`, `group_files`, `process_file_group`), and `main()` with 3 execution modes. A reviewer seeing a single 1,900-line file will question whether the author understands separation of concerns.

- **Anonymous namespace blocks testability.** Every struct and function in the anonymous namespace has internal linkage, meaning nothing can be unit tested from a separate translation unit. This is a significant barrier to adding tests.

- **Global mutable state.** Lines 39-49 define file-scope globals (`g_filter_ticker`, `g_output_dir`, `g_use_parallel`, `g_use_hybrid`, `g_num_threads`, etc.). These are read by functions deep in the call stack, creating hidden dependencies.

- **Hardcoded magic numbers.** `MAX_SYMBOLS = 100000` (used for array pre-allocation), 64 lock shards, 100-fill circular buffer sizes for feature trackers, and various constants are scattered throughout without centralized configuration.

**Refactoring opportunities:**

- Extract `ExecutionModelConfig`, feature trackers, `VirtualOrder`, `StrategyExecState`, `FillRecord`, `SymbolRiskState` into separate headers (e.g., `execution_model.hpp`, `feature_trackers.hpp`, `fill_record.hpp`).
- Extract `PerSymbolSim` into its own file (it is the core simulation unit).
- Extract the hybrid process management (`group_files`, `process_file_group`, shared memory setup) into `process_manager.hpp/cpp`.
- Extract CSV output into `csv_writer.hpp/cpp`.
- Move globals into a `SimConfig` struct passed by const reference.
- Replace the anonymous namespace with a named namespace (e.g., `mmsim`) for named internal linkage where needed, or simply make the types non-anonymous.

#### `src/market_maker.cpp` (484 lines)

**Purpose:** Strategy implementation: spread calculation, toxicity-adjusted quoting, inventory skew, OBI adjustment, order fill handling, and the `OnlineToxicityModel` SGD implementation.

**Issues:**

- Clean and focused. This is the best-organized source file in the project.
- The SGD implementation in `OnlineToxicityModel` is concise and correct.
- No significant issues.

#### `src/market_maker.hpp` (181 lines)

**Purpose:** Header declaring `OnlineToxicityModel`, `ToxicityFeatureVector`, `MarketMakerStats`, `MarketMakerQuote`, and `MarketMakerStrategy`.

**Issues:**

- Clean, well-structured.
- `N_TOXICITY_FEATURES = 8` is appropriately defined here as a constant.
- No significant issues.

#### `src/order_book.hpp` (425 lines)

**Purpose:** `OrderBook` class with `ToxicityMetrics` and `BookStats`, thread-safe getters.

**Issues:**

- **Dead fields in `BookStats`.** Lines 93-100 define eight toxicity timescale fields (`one_ms_toxicity`, `ten_ms_toxicity`, `hundred_ms_toxicity`, `one_second_toxicity`, `ten_seconds_toxicity`, `hundred_seconds_toxicity`, `one_minute_toxicity`, `ten_minutes_toxicity`). These fields are never populated anywhere in the codebase. They appear to be remnants of a planned multi-timeframe toxicity feature that was superseded by the per-level `ToxicityMetrics` approach. They should be removed.

**Refactoring opportunity:**

- Remove the eight dead `BookStats` toxicity timescale fields (lines 93-100).

#### `src/reader.cpp` (775 lines)

**Purpose:** Standalone XDP message parser CLI for inspecting PCAP files.

**Issues:**

- **Duplicated message parsing logic.** `parse_message_simple()` (line 39) and `parse_message_verbose()` (line 303) contain nearly identical `switch` statements covering all XDP message types. The only difference is the verbosity of the output. This is a classic DRY violation.

**Refactoring opportunity:**

- Refactor to a single `parse_message()` with a verbosity parameter, or extract the message-type dispatch into a common function.

#### `src/visualization.cpp` (447 lines)

**Purpose:** ImGui-based order book visualizer using SDL2/OpenGL.

**Issues:**

- **Contains both a class definition and a `main()` function.** The `OrderBookVisualizer` class is defined starting at line 26, and a standalone `main()` function with hardcoded sample data exists at line 411.

- **Included as source by another file.** `visualizer_main.cpp` (line 3) does `#include "visualization.cpp"`. Including a `.cpp` file is a serious anti-pattern.

**Refactoring opportunity:**

- Extract `OrderBookVisualizer` into `order_book_visualizer.hpp` (declaration) and `order_book_visualizer.cpp` (implementation).

#### `src/visualizer_main.cpp` (419 lines)

**Purpose:** Intended as main entry point for visualizer with PCAP support.

**Issues:**

- **`#include "visualization.cpp"` on line 3.** Including a `.cpp` file violates every C++ convention.

- **Broken extern declarations.** Lines 18-24 reference symbols from an older version of `reader.cpp`. The current `reader.cpp` uses an anonymous namespace, making them invisible to extern declarations. These externs would cause linker errors.

- **Not built by CMake.** This file exists in `src/` but is not listed as a source for any CMake target. It is effectively dead code.

**Refactoring opportunity:**

- Delete `visualizer_main.cpp` entirely (dead code with broken references). `visualizer_pcap.cpp` already provides PCAP-based visualization.

#### `src/visualizer_pcap.cpp` (1,865 lines)

**Purpose:** Complete standalone PCAP-based order book visualizer with ImGui.

**Issues:**

- **Duplicate `OrderBookVisualizer` class.** Completely separate from the one in `visualization.cpp`.
- **Debug print statements.** Lines 505-510 contain debug output that fires every 10,000 packets.
- **Extern atomic declarations and definitions separated.** Lines 486-487 vs 577-578.

**Refactoring opportunities:**

- Remove or guard the debug print statements.
- Consolidate the two `OrderBookVisualizer` implementations.

---

### Common Library Headers

#### `src/common/mmap_pcap_reader.hpp` (297 lines)

- **Unused `BatchedPacket` struct.** Lines 289-294 define a struct never instantiated anywhere. Remove it.

#### `src/common/pcap_reader.hpp` (255 lines)

- Clean, well-structured. No significant issues.

#### `src/common/symbol_map.hpp` (87 lines) / `symbol_map.cpp` (164 lines)

- Clean implementation. No significant issues.

#### `src/common/thread_pool.hpp` (150 lines)

- Clean, standard implementation. No significant issues.

#### `src/common/xdp_types.hpp` (174 lines)

- Clean and well-organized. No significant issues.

#### `src/common/xdp_utils.hpp` (124 lines)

- Clean utility functions, all inline. No significant issues.

---

### Python Scripts

#### `scripts/analyze_fills.py` (558 lines)

- Well-structured with clear function separation. Statistical methodology is sound. No significant issues.

#### `scripts/analyze_results.py` (348 lines)

- **Annualized Sharpe ratio.** Line 131 defines `calculate_annualized_sharpe()`. The manuscript explicitly removed annualized Sharpe as meaningless.
- **Unrealistic P&L scaling projections.** Lines 299-314 project P&L with linear scaling.

#### `scripts/parameter_sensitivity.py` (187 lines)

- **Hardcoded data directory.** Line 27. Not configurable via CLI.
- **Hardcoded output path.** Line 161. Not configurable.

#### `scripts/test_hypotheses.py` (302 lines)

- **Hardcoded output path.** Line 247. Not configurable via CLI.

#### `scripts/run_full_day.sh` (243 lines)

- **Largely obsolete.** The hybrid multi-process mode replaces this workflow.

---

### Build System

#### `CMakeLists.txt` (229 lines)

- **SDL2 is FATAL_ERROR even when only building non-visualization targets.**
- **Duplicate OpenGL linking.** Lines 182-188 and lines 197-203 are identical blocks.
- **`visualizer_main.cpp` is orphaned.** Not listed as a source for any CMake target.
- **No option to skip visualization targets.**

---

### Documentation

#### `documentation/ARCHITECTURE.md` (484 lines)

- **Significantly outdated.** Does not mention `market_maker_sim`. "Market Making Strategy Framework" is listed under "Future Enhancements" despite being fully implemented.

#### `documentation/market_maker_manuscript.tex` (987 lines)

- Excellent, thorough manuscript. Presentation-ready.

#### `documentation/hft_architecture_paper.tex` (466 lines)

- **Contains annualized Sharpe ratio** which the primary manuscript explicitly removed.
- **Data characteristics section is outdated.**
- **Hypothesis test results slightly differ** from primary manuscript.

---

## Overall Recommendations by Focus Area

### 1. Code Organization and Modularity

**(a) Decompose `market_maker_sim.cpp` into 5-7 focused files.**

| New File | Extracted From | Approximate Lines |
|----------|---------------|-------------------|
| `execution_model.hpp` | `ExecutionModelConfig`, latency/queue models | 80 |
| `feature_trackers.hpp` | `TradeFlowTracker`, `SpreadTracker`, `MomentumTracker` | 120 |
| `per_symbol_sim.hpp/cpp` | `PerSymbolSim` struct and its methods | 400 |
| `fill_record.hpp` | `FillRecord`, `VirtualOrder`, `StrategyExecState` | 100 |
| `process_manager.hpp/cpp` | `ProcessResults`, `group_files`, `process_file_group`, shared memory | 250 |
| `csv_writer.hpp/cpp` | CSV output functions | 100 |
| `market_maker_sim.cpp` | `main()`, argument parsing, top-level orchestration | 300 |

**(b) Resolve the visualization class duplication.**

**(c) Replace the anonymous namespace** with a named namespace to enable unit testing.

**(d) Eliminate file-scope globals.** Collect them into a `SimConfig` struct.

### 2. Build System Cleanliness

**(a) Make SDL2 optional.** Change `FATAL_ERROR` to `WARNING`, wrap visualizer targets.

**(b) Deduplicate OpenGL linking.**

**(c) Remove or integrate `visualizer_main.cpp`.**

**(d) Add a `requirements.txt`** for Python dependencies.

### 3. Dead Code and Clutter

| File | Item | Lines | Issue |
|------|------|-------|-------|
| `order_book.hpp` | 8 toxicity timescale fields | 93-100 | Never populated |
| `mmap_pcap_reader.hpp` | `BatchedPacket` struct | 289-294 | Never instantiated |
| `visualizer_pcap.cpp` | Debug print statement | 505-510 | Noisy output |
| `visualizer_main.cpp` | Entire file | 1-419 | Dead code, broken externs |
| `reader.cpp` | `parse_message_verbose()` | 303+ | Near-duplicate of `parse_message_simple()` |
| `analyze_results.py` | `calculate_annualized_sharpe()` | 131 | Manuscript says meaningless |
| `run_full_day.sh` | Entire script | 1-243 | Obsolete |
| `hft_architecture_paper.tex` | Annualized Sharpe ratio | 359 | Contradicts primary manuscript |

### 4. File and Directory Structure

Separate generated output from source documentation:
```
documentation/
  manuscripts/
    market_maker_manuscript.tex
    hft_architecture_paper.tex
  architecture/
    ARCHITECTURE.md
output/
  results.txt
  parameter_sensitivity.json
  hypothesis_test_results.json
  analysis_output/
    fills_group_*.csv
    symbols_group_*.csv
    advanced_analysis.json
```

### 5. Protection of Results

**(a) Move generated output to `output/` directory.**
**(b) Add `.gitignore` rules** for intermediate CSVs.
**(c) Add `--output-dir` to scripts** with hardcoded paths.
**(d) Consider a results manifest** with git hash and parameters.

### 6. Presentation Readiness

**Strengths:** Primary manuscript is excellent. Core strategy code is clean. Common library demonstrates good C++ engineering. Statistical analysis is thorough. Hybrid multi-process architecture is impressive. Real results at meaningful scale.

**Weaknesses:** `market_maker_sim.cpp` god-file, `#include "visualization.cpp"` red flag, dead code, outdated `ARCHITECTURE.md`, inconsistent manuscripts.

---

## Priority Action Items

### Immediate (< 30 minutes, high impact)

1. Delete `src/visualizer_main.cpp`
2. Remove dead `BookStats` toxicity timescale fields in `order_book.hpp`
3. Remove `BatchedPacket` in `mmap_pcap_reader.hpp`
4. Remove debug prints in `visualizer_pcap.cpp`
5. Fix `hft_architecture_paper.tex` inconsistencies

### Short-term (1-2 hours, medium impact)

6. Make SDL2 optional in `CMakeLists.txt`
7. Add `--output-dir` arguments to scripts
8. Deprecate `run_full_day.sh`
9. Remove `calculate_annualized_sharpe()` from `analyze_results.py`
10. Move generated output to separate `output/` directory

### Medium-term (4-8 hours, high impact for maintainability)

11. Decompose `market_maker_sim.cpp` into 5-7 focused files
12. Consolidate `OrderBookVisualizer` into a single implementation
13. Refactor `reader.cpp` to eliminate duplicated parsing
14. Update `ARCHITECTURE.md`

---

*Review conducted on the full codebase as of git commit `cd45298` (main branch).*
