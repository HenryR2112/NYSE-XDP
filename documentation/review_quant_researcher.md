# Complete Review: NYSE-XDP Market Maker Simulation

**Reviewer Role**: Senior Quantitative Researcher
**Date**: February 20, 2026
**Scope**: All project files (C++ source, Python scripts, documentation, analysis output)
**Constraint**: Software output must NOT change; review covers organization, clarity, methodology, and presentation only.

---

## Executive Summary

This project implements a toxicity-screened market-making simulation on NYSE XDP data. The research methodology is fundamentally sound: the core statistical claims are supported by appropriate tests, the manuscript is honest about limitations (weak per-fill signal, single-day dataset, marginal profitability), and the parameter sensitivity analysis is thorough. However, there are **six parameter value mismatches** between the manuscript and the C++ defaults, one secondary manuscript (`hft_architecture_paper.tex`) containing **actively misleading stale data**, and two legacy scripts that should be removed or clearly deprecated.

**Overall Assessment**: Strong quantitative work with good intellectual honesty. The issues below are organizational and presentational, not methodological.

---

## 1. Per-Script Statistical Methodology Assessment

### 1.1 `scripts/analyze_fills.py` (557 lines) -- PRIMARY ANALYSIS

**Strengths**:
- Spearman rank correlation is the correct nonparametric choice for toxicity-vs-adverse relationship
- Symbol-level bootstrap (10,000 resamples, seed=42) is well-implemented with proper percentile CIs
- Newey-West HAC with Bartlett kernel correctly accounts for intraday autocorrelation
- Per-feature decomposition provides the key insight (cancel_ratio dominance)

**Issues**:

1. **Population vs. sample standard deviation in Spearman** (lines 179-180): Uses `sum(...) / n` (population variance) rather than `sum(...) / (n-1)`. For n=3,171 this is negligible and conventionally correct for rank correlations -- but a comment would prevent confusion with `test_hypotheses.py` which uses sample stdev.

2. **HAC drops zero-PnL bins silently** (line 442): `nonempty_bins = [p for p in tox_bin_pnl if p != 0.0]` discards time bins with exactly zero PnL. Defensible (zero-PnL bins represent no-activity periods) but should be documented. If any bins had fills that net to exactly $0.00, they would be incorrectly discarded.

3. **HAC t-statistic uses normal approximation** (lines 389-392): For 82 time bins, the normal approximation to the t-distribution is acceptable. The t(81) critical value at 0.05 is 1.990 vs. 1.960 for normal -- negligible given t=5.53.

4. **`from math import erf` inside function body** (lines 190, 256, 388): Minor style issue -- import repeated in three separate functions.

**Verdict**: Methodology is sound. No changes to output required.

### 1.2 `scripts/test_hypotheses.py` (302 lines) -- HYPOTHESIS TESTING

**Strengths**:
- Bootstrap Sharpe difference test (5,000 iterations) is appropriate for H1
- Paired t-test for H2/H3 is the correct choice for matched group-level data
- Binomial test for H4/H5 is exact (not approximate)
- Uses sample stdev (`statistics.stdev`) for Sharpe, consistent with manuscript's 2.031 value

**Issues**:

1. **Dual parsing paths create Sharpe discrepancy**: Two regex patterns (old `Aggregation done:` format, line 42, and new `Group N:` format, lines 50-53) parse into different lists. The Sharpe comes from the old format. This is the source of 2.031 (`test_hypotheses.py`) vs. 2.108 (`parameter_sensitivity.py`) -- different per-group values.

2. **Normal approximation for paired t-test** (lines 128-130): With n=14 groups, the t(13) critical value is 1.771 (one-sided, 0.05) vs. normal 1.645. Technically liberal, but for |t| > 3.4, makes no practical difference.

**Verdict**: Sound methodology. Document which parsing path is authoritative.

### 1.3 `scripts/parameter_sensitivity.py` (187 lines) -- SENSITIVITY GRID

**Strengths**: One-at-a-time sensitivity is standard; default values match C++ exactly; extended ranges test degraded assumptions.

**Issues**: No multi-parameter interaction exploration. Standard for sensitivity analysis but should be noted in the manuscript.

**Verdict**: Appropriate for the stated purpose.

### 1.4 `scripts/analyze_results.py` (348 lines) -- LEGACY

Superseded by `analyze_fills.py` + `test_hypotheses.py`. Contains annualized Sharpe (`daily_sharpe * sqrt(252)`) deliberately removed from manuscript, and misleading P&L scaling projections. **Recommendation**: Delete or add deprecation header.

### 1.5 `scripts/run_full_day.sh` (243 lines) -- OBSOLETE

Completely replaced by hybrid multi-process mode. Processes files sequentially, uses fragile bash arithmetic, has wrong quote size assumption (100 vs. 1,000 shares). **Recommendation**: Delete.

---

## 2. Results Traceability

### 2.1 Manuscript Claims vs. Source Data

Every headline number was verified against its source JSON file:

| Manuscript Claim | Value | Source File | Verified |
|---|---|---|---|
| Baseline Total PnL | -$16,342.28 | `hypothesis_test_results.json` | YES |
| Toxicity Total PnL | $791.71 | `hypothesis_test_results.json` | YES |
| PnL Improvement | $17,133.99 | Computed | YES |
| Baseline fills | 21,233 | `hypothesis_test_results.json` | YES |
| Toxicity fills | 4,671 | `hypothesis_test_results.json` | YES |
| Adverse fills | 1,600 | `hypothesis_test_results.json` | YES |
| Spearman rho | 0.118 | `advanced_analysis.json` (0.11799) | YES |
| Bootstrap CI (total PnL) | [$747, $837] | `advanced_analysis.json` | YES |
| Fraction symbols improved | 47.2% | `advanced_analysis.json` (0.4715) | YES |
| HAC SE inflation | 1.51x | `advanced_analysis.json` (1.5104) | YES |
| HAC t-statistic | 5.53 | `advanced_analysis.json` (5.5338) | YES |
| HAC 95% CI | [$18.76, $39.33] | `advanced_analysis.json` | YES |
| Active symbols | 3,731 | `advanced_analysis.json` | YES |
| Sharpe (toxicity) | 2.031 | `hypothesis_test_results.json` (2.0314) | YES |
| Sharpe (baseline) | -1.454 | `hypothesis_test_results.json` (-1.4538) | YES |
| H2 adverse reduction | 71.9% | `hypothesis_test_results.json` (71.86) | YES |
| H3 inventory variance reduction | 85.6% | `hypothesis_test_results.json` (85.64) | YES |
| Cancel ratio rho | 0.116 | `advanced_analysis.json` (0.11638) | YES |

All headline numbers trace correctly. Rounding is consistent.

### 2.2 Six Parameter Value Mismatches: Manuscript vs. Code

| Parameter | Manuscript Value | Code Default | Manuscript Location | Code Location |
|---|---|---|---|---|
| maker_rebate_per_share | $0.002/share | **$0.0025/share** | tex line 514 | `market_maker_sim.cpp` line 81 |
| clearing_fee_per_share | $0.00015/share | **$0.00008/share** | tex line 514 | `market_maker_sim.cpp` line 83 |
| quote_update_interval | 50 us | **10 us** | tex line 504 | `market_maker_sim.cpp` line 64 |
| max_position_per_symbol | 5,000 shares | **50,000 shares** | tex line 516 | `market_maker_sim.cpp` line 86 |
| max_daily_loss_per_symbol | $500 | **$5,000** | tex line 516 | `market_maker_sim.cpp` line 87 |
| adverse_lookforward_us | 500 us | **250 us** | tex lines 440, 511 | `market_maker_sim.cpp` line 74 |

**Impact**: These do not affect simulation output (code uses its own defaults), but undermine reproducibility. The lookforward window mismatch is most significant -- the manuscript states 500us in Sections 7.4 and 8.1 but the code uses 250us, while Section 9.2.3 correctly says "250us," creating an internal manuscript inconsistency.

---

## 3. Assessment of Two Manuscripts

### 3.1 `market_maker_manuscript.tex` (987 lines) -- PRIMARY

**Quality**: High. Well-structured, intellectually honest. Correctly frames contribution as loss avoidance rather than alpha generation. SGD weight convergence analysis with transparent sample limitations (1.3% of symbols). Online learning negative result honestly reported.

**Issues**: The six parameter mismatches documented above, plus internal lookforward window inconsistency.

### 3.2 `hft_architecture_paper.tex` (466 lines) -- SECONDARY

**Status**: Contains actively misleading data.

1. **Annualized Sharpe 32.248** (line 359): The primary manuscript deliberately removed this with the note "single-day extrapolation is not meaningful." The architecture paper displays it as a headline result.

2. **Stale dataset description** (lines 72-73): Describes "13:30:00 to 13:30:10 Eastern Time, approximately 10 seconds" with "12,502,026 UDP packets." The actual dataset is a full trading day (6.5 hours, 288M packets, 1.27B messages).

3. **Sharpe 2.108** (line 359): Differs from the primary manuscript's authoritative 2.031.

4. **P&L scaling projections** (lines 395-411): Projects annual P&L of $199K-$2M without market impact modeling.

5. **~60% content duplication** from the primary manuscript with stale numbers.

**Recommendation**: Either fully sync with the primary manuscript (removing annualized Sharpe and scaling projections) or retire as a development artifact.

---

## 4. Dead / Obsolete Analysis

| File | Status | Reason | Recommendation |
|---|---|---|---|
| `scripts/run_full_day.sh` | OBSOLETE | Replaced by hybrid multi-process mode | Delete |
| `scripts/analyze_results.py` | LEGACY | Superseded by analyze_fills.py + test_hypotheses.py | Delete or deprecate |
| `hft_architecture_paper.tex` | STALE | Misleading numbers (annualized Sharpe, 10-sec dataset) | Sync or retire |
| `documentation/ARCHITECTURE.md` | STALE | Describes reader/visualizer, not market_maker_sim | Update or remove |

---

## 5. Code-Manuscript Alignment Issues

### 5.1 Toxicity Level Count Discrepancy

- `build_feature_vector()` in `market_maker_sim.cpp` line 373: `constexpr int levels = 3` -- matches manuscript
- `calculate_toxicity_adjusted_spread()` in `market_maker.cpp`: uses **5 levels** -- not documented
- `get_average_toxicity()` in `market_maker.cpp`: uses 3 levels -- matches manuscript

Since spread adjustment has negligible effect (<$4 variation across 8x kappa change), this is inconsequential but should be corrected for consistency.

### 5.2 should_quote Threshold

The code uses `expected_pnl > 0.0005` but the manuscript writes epsilon_min without specifying the numeric value. Should be documented.

### 5.3 SGD Learned Weights Output

The per-group weight output is ad hoc (interleaved with aggregation output). A structured JSON format would improve traceability for manuscript Table 6.

---

## 6. Specific Recommendations

### Priority 1 (High -- Manuscript Correctness)

1. **Fix the six parameter mismatches**: Update manuscript to match code defaults. The lookforward window (250us vs. 500us) is especially important.

2. **Resolve Sharpe discrepancy** (2.031 vs. 2.108): Document that 2.031 from `test_hypotheses.py` is authoritative.

3. **Address `hft_architecture_paper.tex`**: Remove annualized Sharpe, fix dataset description, align numbers -- or retire.

### Priority 2 (Medium -- Reproducibility)

4. **Create results manifest**: JSON mapping manuscript claims to source files.

5. **Deprecate/remove legacy scripts**: `run_full_day.sh` and `analyze_results.py`.

6. **Document HAC zero-bin filtering**: Comment in `analyze_fills.py` line 442.

### Priority 3 (Low -- Improvements)

7. Note one-at-a-time sensitivity limitation in manuscript.
8. Add epsilon_min value (0.0005) to manuscript.
9. Standardize toxicity level counts (3 vs. 5).
10. Structured JSON weight output for Table 6 traceability.
11. Update `ARCHITECTURE.md` for current system.
12. Module-level `from math import erf` in `analyze_fills.py`.

---

## 7. What Does NOT Need Changing

- Spearman rank correlation implementation (population variance is conventional)
- Bootstrap methodology (10,000 resamples, fixed seed, percentile CIs)
- Newey-West HAC (Bartlett kernel, automatic lag, 1.51x inflation is plausible)
- Paired t-test for H2/H3 (appropriate for matched data)
- SGD model architecture (8-feature logistic with Welford normalization)
- Manuscript framing (loss-avoidance narrative, honest weak-signal treatment)
- Parameter sensitivity results (JSON matches all manuscript table values)

---

*Review prepared from comprehensive reading of all project files. No simulation output was modified or re-generated.*
