# Managing Director Review: NYSE-XDP Repository

**Reviewer:** Executive-level presentation assessment
**Date:** February 20, 2026
**Scope:** Full repository review -- every tracked file outside `thirdparty/imgui`
**Constraint:** Software output must not change. This review concerns presentation, narrative, and organization only.

---

## I. First Impressions Assessment

This is a serious, technically accomplished project. Within the first five minutes of reading, three things are clear: (1) the author understands market microstructure at a level beyond textbook fluency, (2) the C++ is production-grade, not academic-grade, and (3) the statistical discipline is unusually rigorous for a work sample -- negative results are documented honestly rather than hidden. The combination of real NYSE PCAP data, a from-scratch XDP parser, a genuine market-making simulation, and honest statistical evaluation puts this well above the typical "I backtested a strategy" portfolio piece.

However, the repository as presented is a working research directory, not a curated deliverable. It contains the bones of an outstanding work sample buried under layers of intermediate artifacts, duplicate documents, debug leftovers, and organizational noise that would dilute its impact in front of a hiring committee. The signal is strong. The noise is unnecessary.

---

## II. Signal-to-Noise Analysis

### What Is Signal (Keep, Promote, Polish)

| File/Component | Verdict | Notes |
|---|---|---|
| `src/market_maker_sim.cpp` | **Core signal** | The crown jewel. Fork-based parallelism over 74GB of real exchange data, per-symbol simulation, SGD toxicity model. Professional quality. |
| `src/market_maker.cpp` / `.hpp` | **Core signal** | Clean strategy abstraction. OnlineToxicityModel with SGD, proper `[[nodiscard]]` usage, well-separated concerns. |
| `src/order_book.hpp` | **Core signal** | Thread-safe order book with toxicity metrics. Good use of mutex, proper const correctness. |
| `src/common/mmap_pcap_reader.hpp` | **Core signal** | Zero-copy mmap with MADV_SEQUENTIAL. This is the kind of systems thinking firms want to see. |
| `src/common/thread_pool.hpp` | **Core signal** | Work-stealing thread pool with parallel_for. Compact, correct. |
| `src/common/xdp_types.hpp` / `xdp_utils.hpp` | **Core signal** | Packed structs, constexpr functions, proper endian handling. Shows protocol-level competence. |
| `src/common/pcap_reader.hpp` | **Core signal** | Proper network header parsing (Ethernet/IP/UDP/XDP). |
| `src/common/symbol_map.hpp` / `.cpp` | **Core signal** | Solid CSV parser with quoted field support. |
| `src/reader.cpp` | **Core signal** | Complete XDP message parser for all message types 100-223. |
| `documentation/market_maker_manuscript.tex` | **Core signal** | The primary intellectual deliverable. Formal mathematical framework, honest about limitations, proper hypothesis testing. Needs polish (see below). |
| `scripts/analyze_fills.py` | **Core signal** | Standard-library-only statistical analysis. Spearman, bootstrap, HAC. Demonstrates quantitative rigor. |
| `scripts/test_hypotheses.py` | **Core signal** | Five formal hypothesis tests. This is what separates a serious quant from someone who reports a single backtest number. |
| `scripts/parameter_sensitivity.py` | **Core signal** | Systematic grid search. One-at-a-time sensitivity analysis. |
| `documentation/hypothesis_test_results.json` | **Core signal** | Machine-readable hypothesis test output. Useful as an appendix reference. |
| `documentation/parameter_sensitivity.json` | **Core signal** | Machine-readable sensitivity results. Same. |
| `CMakeLists.txt` | **Core signal** | Clean four-target build. Aggressive optimization flags appropriate for HFT context. |
| `README.md` | **Signal, needs editing** | Extremely thorough. Too thorough for a first impression -- more of a reference manual than a pitch. |
| `src/visualization.cpp` | **Supporting** | Order book visualizer with ImGui. Nice demo capability but not the main story. |
| `src/visualizer_pcap.cpp` | **Supporting** | Full-featured PCAP replay visualizer with toxicity overlay, playback controls, ring buffer. Impressive but secondary. |

### What Is Noise (Archive, Remove, Restructure)

| File/Component | Verdict | Reason |
|---|---|---|
| `documentation/hft_architecture_paper.tex` | **Archive or delete** | Significant content overlap with `market_maker_manuscript.tex`. Worse: contains an "Annualized Sharpe Ratio (est.): 32.248" and a "P&L Scaling Projections" table extrapolating to $1.99M annual, which the manuscript correctly removed as indefensible from single-day data. This paper actively undermines the credibility that the manuscript builds. If a reviewer finds this file, they will question the author's judgment. |
| `documentation/ARCHITECTURE.md` | **Rewrite or delete** | Describes three executables (reader, visualizer, visualizer_pcap) but does not mention `market_maker_sim` -- the central component. Lists "Future Enhancements" for features that already exist. This file is a snapshot of an earlier project state and will confuse anyone reading it. |
| `documentation/results.txt` | **Archive** | Raw simulation output including 14 repetitions of "Loaded 12249 symbol mappings." This is a log file, not a results document. The useful numbers are already in `hypothesis_test_results.json`. |
| `documentation/analysis_output/*.csv` | **Archive** | 28 CSV files (14 fills + 14 symbols) totaling several MB. These are intermediate analysis artifacts. They belong in a reproducibility archive, not the main repository tree. |
| `imgui.ini` | **Remove from tracking** | ImGui window layout state file. Debug artifact. Should be in `.gitignore`. |
| `reader` | **Remove from tracking** | This appears to be a committed binary. Binaries should never be in version control. |
| `notes.ipynb` | **Archive** | A single-cell Jupyter notebook that parses the symbol file. This was a one-time data preparation step. It tells a reviewer "I forgot to clean up my scratch work." |
| `.vscode/launch.json` | **Remove or archive** | Six debug configurations with input prompts. Editor-specific, not portable. |
| `.vscode/tasks.json` | **Remove or archive** | Fourteen build/run tasks. Same issue. |
| `scripts/analyze_results.py` | **Archive** | Contains `calculate_annualized_sharpe` and "Scaling Projections" that the manuscript deliberately avoids. Same credibility risk as `hft_architecture_paper.tex`. |
| `scripts/run_full_day.sh` | **Demote** | Useful operationally but does not contribute to the work sample narrative. |
| `data_sheets/*.pdf` | **Assess IP risk** | NYSE XDP specification PDFs. These are likely proprietary NYSE documentation. Including them in a public or semi-public repository raises IP concerns. |
| `documentation/strategy_proposal.pdf` | **Assess content** | Tracked in git but not examined in this review. If it contains speculative claims similar to the architecture paper, it poses the same credibility risk. |

### Signal-to-Noise Ratio

Of the approximately 50 non-thirdparty tracked files, roughly 30 are core signal. The remaining 20 range from harmless clutter to actively damaging. The ratio is better than most research repositories, but the damaging items (architecture paper with annualized Sharpe, outdated ARCHITECTURE.md, committed binary) need urgent attention.

---

## III. README and Documentation Quality

### README.md

**Strengths:**
- Comprehensive CLI reference tables for all four build targets
- Correct build instructions with CMake
- Architecture description covers the hybrid multi-process design
- XDP protocol section demonstrates domain expertise
- Project structure tree is accurate

**Problems:**
- It reads like a reference manual, not an executive summary. The first thing a reviewer sees should answer "What is this project and why should I care?" within 30 seconds. Currently, the first substantive content is the project structure tree.
- No results summary in the README. The headline numbers ($792 toxicity PnL vs -$16,342 baseline, Sharpe 2.03 vs -1.45, rho=0.118 with honest assessment) should be front and center.
- The "Features" list buries the intellectual contribution under implementation details. "Multi-threaded PCAP processing" is a feature. "Toxicity-screened market making with formal statistical validation" is the contribution.
- No mention of the negative result (online learning did not improve over static initialization). This is a mark of intellectual honesty that should be highlighted, not hidden in the manuscript.

**Recommendation:** Restructure the README into three tiers:
1. **Top:** One-paragraph elevator pitch + key results table (3 lines)
2. **Middle:** How to build and run (keep existing, compress slightly)
3. **Bottom:** Detailed architecture and protocol notes (keep existing, move lower)

### market_maker_manuscript.tex

**Strengths:**
- Formal mathematical framework (Avellaneda-Stoikov extension with toxicity)
- Five hypothesis tests with proper p-values
- Honest about weak per-fill correlation (rho=0.118) while demonstrating aggregate effectiveness
- Negative result documented (online learning, per-feature analysis)
- The framing of "loss avoided" ($16,342) being more informative than "profit earned" ($792) is sophisticated and correct
- Learned weights table across 50 symbols and 9 groups
- Cancel ratio dominance finding is well-supported

**Problems:**
- Author field shows "(Author)" -- placeholder not filled in
- The paper is long. For a work sample, a reader will likely not get past page 5. The most compelling content (hypothesis tests, sensitivity analysis, negative results) is buried deep.
- No abstract or executive summary at the very top that gives the punchline
- Single-day limitation is acknowledged but could be more prominently flagged upfront rather than discovered later

**Recommendation:** Add a structured abstract with explicit numerical claims. Consider whether the paper could be tightened to 15-20 pages.

### hft_architecture_paper.tex

**Verdict: This file should not exist in the repository in its current form.**

It contains:
- "Annualized Sharpe Ratio (est.): 32.248" -- extrapolating a single-day result to annual frequency is indefensible, and the manuscript correctly removed this
- "P&L Scaling Projections" table claiming $1.99M annual potential -- this is fantasy from one day of data
- Substantial content overlap with the manuscript, creating confusion about which is authoritative

If a sophisticated reviewer finds this file, it will undo the credibility built by the manuscript's careful statistical discipline. Either delete it, move it to an archive branch, or strip it down to architecture-only content with no performance claims.

### ARCHITECTURE.md

**Verdict: Outdated and misleading.**

This document describes a project with three executables (reader, visualizer, visualizer_pcap) and does not mention `market_maker_sim` -- the entire point of the project. It contains a "Future Enhancements" section listing capabilities that already exist in the codebase. A reviewer reading this will think the project is less complete than it actually is.

Either delete it, or rewrite it to accurately describe the current four-target architecture with `market_maker_sim` as the primary component.

---

## IV. Results Organization and Protection

### Key Numbers

The authoritative results chain is:

1. `market_maker_sim` binary produces stdout/stderr with per-group statistics
2. `documentation/results.txt` captures raw output (verbose, includes repeated log lines)
3. `scripts/test_hypotheses.py` processes results into `documentation/hypothesis_test_results.json`
4. `scripts/analyze_fills.py` processes CSVs into `documentation/analysis_output/advanced_analysis.json`
5. `scripts/parameter_sensitivity.py` processes grid search into `documentation/parameter_sensitivity.json`
6. `documentation/market_maker_manuscript.tex` cites these numbers

**The numbers are preserved.** The JSON files are machine-readable and checked into git. The pipeline from simulation to analysis to manuscript is traceable. This is good.

**Concerns:**
- `results.txt` is the weakest link. It is verbose raw output, not a curated results file. If someone overwrites it with a new run, the JSON files become inconsistent unless the full pipeline is re-run.
- There is no Makefile or script that runs the full pipeline (simulate -> analyze -> test hypotheses -> sensitivity) as a single reproducible workflow.
- The CSV files in `analysis_output/` are large intermediate artifacts. If they are needed for reproducibility, document that explicitly. If not, they are clutter.

**Recommendation:** Create a single `scripts/reproduce.sh` that chains the full pipeline with checksums or hash verification of outputs.

### Key Results Summary (for reference)

| Metric | Value |
|---|---|
| Baseline Total PnL | -$16,342.28 |
| Toxicity Total PnL | $791.71 |
| PnL Improvement | $17,133.99 |
| Sharpe (toxicity) | 2.031 |
| Sharpe (baseline) | -1.454 |
| Spearman rho | 0.118 (per-fill), significant aggregate |
| Adverse Selection Reduction | 71.9% (p=0.00025) |
| Inventory Variance Reduction | 85.6% (p=0.00019) |
| Cross-Sectional Robustness | 14/14 groups profitable |
| Symbol Bootstrap 95% CI | [$747, $837] |
| HAC t-statistic | 5.53, p < 1e-7 |
| Online Learning Improvement | None (negative result) |
| Cancel Ratio Dominance | rho=0.116 of 0.118 total |

---

## V. Professional Polish Assessment

### Code Quality

**C++ (Excellent):**
- Consistent C++17 idioms (`std::optional`, `std::string_view`, structured bindings)
- Proper use of `[[nodiscard]]`, `constexpr`, packed structs
- Thread safety with `std::mutex` where needed, atomics where appropriate
- RAII patterns, no raw `new`/`delete`
- Clear naming conventions throughout
- Memory-mapped I/O with `MADV_SEQUENTIAL` shows systems-level thinking
- The `PerSymbolSim` struct in `market_maker_sim.cpp` is well-designed

**Python (Good):**
- Standard library only -- no numpy/pandas/scipy dependencies. Deliberate and defensible choice.
- Clean function organization in `analyze_fills.py` and `test_hypotheses.py`
- Proper use of `statistics.stdev` (sample, n-1) after correcting from `pstdev`

**Visualization (Solid but secondary):**
- `visualizer_pcap.cpp` is the most feature-rich: ring buffer, checkpointing, toxicity-over-time graph, trade markers with fade effects
- `visualization.cpp` is a simpler version with hardcoded test data in `main()`
- `visualizer_main.cpp` `#include`s `visualization.cpp` as a source file -- code smell

### Debug Artifacts and Clutter

| Item | Issue |
|---|---|
| `imgui.ini` | Window layout state. Should be gitignored. |
| `reader` (binary) | Committed binary in root. Remove from tracking. |
| `notes.ipynb` | One-cell scratch notebook. Archive. |
| `visualizer_pcap.cpp` line 506 | `std::cout << "Debug: Parsed ..."` -- debug output left in code. |
| `visualizer_pcap.cpp` lines 264-276 | Debug block for sample symbols. Should be removed or behind a flag. |
| `visualization.cpp` main() | Hardcoded test orders. Test scaffolding. |
| `visualizer_main.cpp` line 3 | `#include "visualization.cpp"` -- including a source file. |

---

## VI. Specific Recommendations for Presentation-Ready State

### Priority 1: Remove Credibility Risks (Do This First)

1. **Delete or archive `documentation/hft_architecture_paper.tex`**
2. **Delete or rewrite `documentation/ARCHITECTURE.md`**
3. **Remove `scripts/analyze_results.py` or strip it**
4. **Remove the committed binary `reader`** from git tracking
5. **Remove `imgui.ini`** from git tracking. Add to `.gitignore`.

### Priority 2: Restructure for Impact (Do This Second)

6. **Rewrite the top of README.md** -- Lead with elevator pitch + results table
7. **Add an abstract to the manuscript** with explicit numerical claims
8. **Fill in the author field** in `market_maker_manuscript.tex`
9. **Move intermediate analysis CSVs** to an archive or separate branch

### Priority 3: Polish Details (Do This Last)

10. **Archive `notes.ipynb`**
11. **Consolidate visualization files** -- Keep `visualizer_pcap.cpp` only
12. **Remove debug output** from `visualizer_pcap.cpp`
13. **Remove `.vscode/launch.json` and `.vscode/tasks.json`** from git tracking
14. **Add to `.gitignore`**: `imgui.ini`, `*.ipynb`, `.vscode/launch.json`, `.vscode/tasks.json`
15. **Create a `scripts/reproduce.sh`** for full pipeline reproducibility
16. **Consider the `data_sheets/` PDFs** -- verify IP/distribution rights
17. **Consider `documentation/strategy_proposal.pdf`** -- verify no speculative claims

---

## VII. IP Awareness Assessment

### Credentials and Secrets
- **No API keys found.** Clean.
- **No hardcoded absolute paths** in source code. All paths are relative. Clean.
- **No personal information** beyond the "(Author)" placeholder. Clean.

### Proprietary Data
- **`data/` directory is gitignored.** The 74GB of PCAP data is not committed. Correct.
- **`data_sheets/*.pdf` ARE tracked in git.** NYSE XDP specification documents. Distribution terms should be verified.
- **`documentation/strategy_proposal.pdf`** is tracked. Contents not examined. Verify no proprietary information.

### Third-Party Code
- ImGui is included as a full source tree rather than as a git submodule. MIT-licensed, so legally fine, but inflates repo size. Converting to a proper submodule would be cleaner.

---

## VIII. What to Archive, Promote, and Rewrite

### Archive (move to archive branch or delete)

| File | Reason |
|---|---|
| `documentation/hft_architecture_paper.tex` | Contradicts manuscript's intellectual discipline |
| `documentation/ARCHITECTURE.md` | Describes wrong project. Outdated. |
| `documentation/results.txt` | Raw log output. Numbers preserved in JSON files. |
| `documentation/analysis_output/*.csv` (28 files) | Intermediate artifacts |
| `scripts/analyze_results.py` | Contains scaling projections manuscript deliberately avoids |
| `notes.ipynb` | Scratch notebook |
| `visualization.cpp` | Superseded by `visualizer_pcap.cpp` |
| `visualizer_main.cpp` | Superseded. Includes `.cpp` file. |
| `imgui.ini` | Debug artifact |
| `reader` (binary) | Committed build artifact |
| `.vscode/launch.json` | Personal editor config |
| `.vscode/tasks.json` | Personal editor config |

### Promote (make more visible)

| Content | How |
|---|---|
| Key results (Sharpe 2.03, 14/14 groups, $17K improvement) | Front and center in README |
| Negative result (online learning ineffective) | Highlight in README and manuscript abstract |
| Cancel ratio dominance finding | Promote to a headline finding |
| Hypothesis testing framework | Mention in README as a distinguishing feature |
| mmap + fork parallelism architecture | Elevate in README as systems engineering demonstration |

### Rewrite

| File | What Needs Changing |
|---|---|
| `README.md` (top section) | Lead with impact, not file structure. Results table above the fold. |
| `market_maker_manuscript.tex` (abstract, author) | Add structured abstract. Fill in author. |
| `.gitignore` | Add `imgui.ini`, `*.ipynb`, `.vscode/launch.json`, `.vscode/tasks.json` |

---

## IX. Summary Judgment

**The intellectual content of this project is exceptional for a work sample.** It demonstrates:
- Real-world data engineering (74GB PCAP parsing, XDP protocol implementation)
- Systems programming (mmap, fork, thread pools, lock-free-where-possible design)
- Quantitative modeling (Avellaneda-Stoikov extension, SGD logistic model)
- Statistical rigor (five hypothesis tests, bootstrap CIs, HAC standard errors, honest negative results)
- Intellectual honesty (rho=0.118 acknowledged as weak, online learning documented as ineffective, single-day limitation flagged)

**The presentation undermines the content.** A reviewer who opens the repository will see 50+ files including a debug binary, a scratch notebook, an outdated architecture document, and a second paper that makes claims the primary paper carefully avoids. They will have to work to find the signal. They should not have to.

**The gap between "working research directory" and "presentation-ready work sample" is approximately one focused day of cleanup.** Nothing in the code needs to change. No results need to be altered. The simulation output is preserved. What needs to happen is curation: remove the noise, promote the signal, and let the excellent work speak for itself.

The manuscript alone, cleaned up with a proper abstract and author, is a publishable-quality paper on toxicity-screened market making. The codebase alone is a compelling demonstration of systems and quantitative engineering. Together, presented cleanly, this is a top-tier work sample. The only thing standing in the way is organization.

---

*Review complete. No software changes recommended. All recommendations concern presentation, organization, and credibility protection.*
