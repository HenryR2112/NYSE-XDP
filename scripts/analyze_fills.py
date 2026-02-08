#!/usr/bin/env python3
"""
Advanced Statistical Analysis for Market Maker Simulation.

Reads per-fill and per-symbol CSV output from the simulation to perform:
1. Direct toxicity score validation (realized adverse vs toxicity deciles)
2. Symbol-level bootstrap confidence intervals
3. Spearman rank correlation of toxicity vs adverse selection
4. HAC (Newey-West) standard errors for time-series PnL

Usage:
  python3 scripts/analyze_fills.py --output-dir <dir_with_csvs>

The simulation must have been run with --output-dir to produce the CSVs.
"""
import os
import sys
import csv
import json
import math
import random
import statistics
from collections import defaultdict
from pathlib import Path
from typing import List, Dict, Tuple


FEATURE_NAMES = [
    'cancel_ratio', 'ping_ratio', 'odd_lot_ratio', 'precision_ratio',
    'resistance_ratio', 'trade_flow_imbalance', 'spread_change_rate', 'price_momentum',
]


def load_fills(output_dir: str) -> List[dict]:
    """Load all per-fill CSV files from output directory."""
    fills = []
    for path in sorted(Path(output_dir).glob("fills_group_*.csv")):
        with open(path, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                fill = {
                    'group': int(row['group']),
                    'symbol': int(row['symbol']),
                    'ticker': row['ticker'],
                    'strategy': row['strategy'],
                    'fill_time_ns': int(row['fill_time_ns']),
                    'fill_price': float(row['fill_price']),
                    'fill_qty': int(row['fill_qty']),
                    'is_buy': row['is_buy'] == '1',
                    'mid_price_at_fill': float(row['mid_price_at_fill']),
                    'toxicity_at_fill': float(row['toxicity_at_fill']),
                    'adverse_measured': row.get('adverse_measured', '1') == '1',
                    'adverse_pnl': float(row['adverse_pnl']),
                }
                # Parse per-feature columns (present in online learning runs)
                for fname in FEATURE_NAMES:
                    if fname in row:
                        fill[fname] = float(row[fname])
                fills.append(fill)
    return fills


def load_symbols(output_dir: str) -> List[dict]:
    """Load all per-symbol CSV files from output directory."""
    symbols = []
    for path in sorted(Path(output_dir).glob("symbols_group_*.csv")):
        with open(path, 'r') as f:
            reader = csv.DictReader(f)
            for row in reader:
                symbols.append({
                    'group': int(row['group']),
                    'symbol_index': int(row['symbol_index']),
                    'ticker': row['ticker'],
                    'baseline_pnl': float(row['baseline_pnl']),
                    'toxicity_pnl': float(row['toxicity_pnl']),
                    'improvement': float(row['improvement']),
                    'baseline_fills': int(row['baseline_fills']),
                    'toxicity_fills': int(row['toxicity_fills']),
                    'quotes_suppressed': int(row['quotes_suppressed']),
                    'baseline_adverse_pnl': float(row['baseline_adverse_pnl']),
                    'toxicity_adverse_pnl': float(row['toxicity_adverse_pnl']),
                    'baseline_inv_var': float(row['baseline_inv_var']),
                    'toxicity_inv_var': float(row['toxicity_inv_var']),
                })
    return symbols


# ============================================================================
# 1. Direct Toxicity Score Validation
# ============================================================================

def toxicity_decile_analysis(fills: List[dict]) -> dict:
    """
    Bin fills into toxicity deciles and compute average adverse selection per decile.
    This validates whether higher toxicity actually predicts worse fills.
    """
    # Filter to toxicity strategy fills with measured adverse selection
    tox_fills = [f for f in fills if f['strategy'] == 'toxicity' and f['adverse_measured']]

    if len(tox_fills) < 20:
        return {'testable': False, 'reason': 'Insufficient toxicity fills with adverse data'}

    # Also include baseline fills for comparison
    base_fills = [f for f in fills if f['strategy'] == 'baseline' and f['adverse_measured']]

    # Sort by toxicity score
    tox_fills.sort(key=lambda f: f['toxicity_at_fill'])

    # Create deciles
    n = len(tox_fills)
    decile_size = max(1, n // 10)
    deciles = []

    for i in range(10):
        start = i * decile_size
        end = min((i + 1) * decile_size, n) if i < 9 else n
        bucket = tox_fills[start:end]
        if not bucket:
            continue

        avg_toxicity = statistics.mean(f['toxicity_at_fill'] for f in bucket)
        avg_adverse = statistics.mean(abs(f['adverse_pnl']) for f in bucket)
        total_adverse = sum(abs(f['adverse_pnl']) for f in bucket)
        count = len(bucket)

        deciles.append({
            'decile': i + 1,
            'n_fills': count,
            'avg_toxicity': avg_toxicity,
            'min_toxicity': min(f['toxicity_at_fill'] for f in bucket),
            'max_toxicity': max(f['toxicity_at_fill'] for f in bucket),
            'avg_adverse_pnl': avg_adverse,
            'total_adverse_pnl': total_adverse,
        })

    return {
        'testable': True,
        'n_fills_analyzed': len(tox_fills),
        'n_baseline_fills': len(base_fills),
        'deciles': deciles,
    }


def spearman_rank_correlation(fills: List[dict]) -> dict:
    """
    Compute Spearman rank correlation between toxicity score and adverse selection.
    """
    # Use all measured fills (including those with zero adverse movement)
    valid = [f for f in fills if f['adverse_measured']]
    if len(valid) < 10:
        return {'testable': False, 'reason': 'Insufficient fills'}

    toxicities = [f['toxicity_at_fill'] for f in valid]
    adverses = [abs(f['adverse_pnl']) for f in valid]
    n = len(toxicities)

    # Rank the values
    def rank(values):
        indexed = sorted(enumerate(values), key=lambda x: x[1])
        ranks = [0.0] * len(values)
        i = 0
        while i < len(indexed):
            j = i
            while j < len(indexed) and indexed[j][1] == indexed[i][1]:
                j += 1
            avg_rank = (i + j + 1) / 2.0  # 1-based average rank for ties
            for k in range(i, j):
                ranks[indexed[k][0]] = avg_rank
            i = j
        return ranks

    rank_tox = rank(toxicities)
    rank_adv = rank(adverses)

    # Spearman correlation = Pearson of ranks
    mean_rt = statistics.mean(rank_tox)
    mean_ra = statistics.mean(rank_adv)
    cov = sum((rt - mean_rt) * (ra - mean_ra) for rt, ra in zip(rank_tox, rank_adv)) / n
    std_rt = math.sqrt(sum((rt - mean_rt) ** 2 for rt in rank_tox) / n)
    std_ra = math.sqrt(sum((ra - mean_ra) ** 2 for ra in rank_adv) / n)

    if std_rt == 0 or std_ra == 0:
        return {'testable': True, 'rho': 0.0, 'n': n, 'note': 'zero variance'}

    rho = cov / (std_rt * std_ra)

    # Approximate t-test for significance
    t_stat = rho * math.sqrt((n - 2) / (1 - rho ** 2)) if abs(rho) < 1.0 else float('inf')
    # Normal approximation for p-value
    from math import erf
    z = abs(t_stat)
    p_value = 2 * (1 - 0.5 * (1 + erf(z / math.sqrt(2))))

    return {
        'testable': True,
        'spearman_rho': rho,
        'n_fills': n,
        't_statistic': t_stat,
        'p_value_two_sided': p_value,
        'significant_at_0.05': p_value < 0.05,
    }


def per_feature_correlation(fills: List[dict]) -> dict:
    """
    Compute Spearman rank correlation between each individual feature and adverse_pnl.
    Shows which features are most predictive of adverse selection.
    """
    tox_fills = [f for f in fills if f['strategy'] == 'toxicity' and f['adverse_measured']]

    if len(tox_fills) < 20:
        return {'testable': False, 'reason': 'Insufficient fills'}

    # Check if feature columns are present
    has_features = all(fname in tox_fills[0] for fname in FEATURE_NAMES)
    if not has_features:
        return {'testable': False, 'reason': 'Feature columns not present in CSV (run with --online-learning)'}

    adverses = [abs(f['adverse_pnl']) for f in tox_fills]
    n = len(adverses)

    def rank(values):
        indexed = sorted(enumerate(values), key=lambda x: x[1])
        ranks = [0.0] * len(values)
        i = 0
        while i < len(indexed):
            j = i
            while j < len(indexed) and indexed[j][1] == indexed[i][1]:
                j += 1
            avg_rank = (i + j + 1) / 2.0
            for k in range(i, j):
                ranks[indexed[k][0]] = avg_rank
            i = j
        return ranks

    rank_adv = rank(adverses)

    feature_results = []
    for fname in FEATURE_NAMES:
        values = [f[fname] for f in tox_fills]
        rank_feat = rank(values)

        mean_rf = statistics.mean(rank_feat)
        mean_ra = statistics.mean(rank_adv)
        cov = sum((rf - mean_rf) * (ra - mean_ra) for rf, ra in zip(rank_feat, rank_adv)) / n
        std_rf = math.sqrt(sum((rf - mean_rf) ** 2 for rf in rank_feat) / n)
        std_ra = math.sqrt(sum((ra - mean_ra) ** 2 for ra in rank_adv) / n)

        if std_rf == 0 or std_ra == 0:
            rho = 0.0
            t_stat = 0.0
            p_value = 1.0
        else:
            rho = cov / (std_rf * std_ra)
            t_stat = rho * math.sqrt((n - 2) / (1 - rho ** 2)) if abs(rho) < 1.0 else float('inf')
            from math import erf
            z = abs(t_stat)
            p_value = 2 * (1 - 0.5 * (1 + erf(z / math.sqrt(2))))

        feature_results.append({
            'feature': fname,
            'spearman_rho': rho,
            'abs_rho': abs(rho),
            't_statistic': t_stat,
            'p_value': p_value,
            'significant': p_value < 0.05,
            'mean_value': statistics.mean(values),
            'std_value': statistics.pstdev(values) if len(values) > 1 else 0.0,
        })

    # Sort by absolute rho descending
    feature_results.sort(key=lambda x: x['abs_rho'], reverse=True)

    return {
        'testable': True,
        'n_fills': n,
        'features': feature_results,
    }


# ============================================================================
# 2. Symbol-Level Bootstrap
# ============================================================================

def symbol_bootstrap(symbols: List[dict], n_bootstrap=10000, seed=42) -> dict:
    """
    Bootstrap confidence intervals using per-symbol P&L data.
    Symbols have a stronger independence argument than file groups.
    """
    random.seed(seed)

    # Filter to symbols with at least one fill in either strategy
    active = [s for s in symbols if s['baseline_fills'] > 0 or s['toxicity_fills'] > 0]
    n = len(active)

    if n < 10:
        return {'testable': False, 'reason': f'Only {n} active symbols'}

    # Observed statistics
    tox_pnls = [s['toxicity_pnl'] for s in active]
    base_pnls = [s['baseline_pnl'] for s in active]
    improvements = [s['improvement'] for s in active]

    obs_mean_improvement = statistics.mean(improvements)
    obs_total_tox = sum(tox_pnls)
    obs_total_base = sum(base_pnls)

    # Fraction of symbols where toxicity outperforms
    n_positive = sum(1 for imp in improvements if imp > 0)
    fraction_positive = n_positive / n

    # Bootstrap
    boot_means = []
    boot_totals_tox = []
    boot_totals_base = []
    boot_fractions = []

    for _ in range(n_bootstrap):
        idxs = [random.randrange(n) for _ in range(n)]
        sample_imp = [improvements[i] for i in idxs]
        sample_tox = [tox_pnls[i] for i in idxs]
        sample_base = [base_pnls[i] for i in idxs]

        boot_means.append(statistics.mean(sample_imp))
        boot_totals_tox.append(sum(sample_tox))
        boot_totals_base.append(sum(sample_base))
        boot_fractions.append(sum(1 for s in sample_imp if s > 0) / n)

    boot_means.sort()
    boot_totals_tox.sort()
    boot_totals_base.sort()

    ci_lo = int(0.025 * n_bootstrap)
    ci_hi = int(0.975 * n_bootstrap)

    # P-value for improvement > 0
    p_value = sum(1 for m in boot_means if m <= 0) / n_bootstrap

    return {
        'testable': True,
        'n_symbols': n,
        'n_bootstrap': n_bootstrap,
        'observed_mean_improvement': obs_mean_improvement,
        'observed_total_toxicity_pnl': obs_total_tox,
        'observed_total_baseline_pnl': obs_total_base,
        'fraction_symbols_improved': fraction_positive,
        'bootstrap_ci_95_improvement': [boot_means[ci_lo], boot_means[ci_hi]],
        'bootstrap_ci_95_toxicity_pnl': [boot_totals_tox[ci_lo], boot_totals_tox[ci_hi]],
        'bootstrap_ci_95_baseline_pnl': [boot_totals_base[ci_lo], boot_totals_base[ci_hi]],
        'p_value_improvement_gt_zero': p_value,
    }


# ============================================================================
# 3. HAC (Newey-West) Standard Errors
# ============================================================================

def newey_west_se(values: List[float], max_lag: int = None) -> dict:
    """
    Compute Newey-West HAC standard errors for a time series of P&L values.
    Accounts for serial autocorrelation in the data.
    """
    n = len(values)
    if n < 5:
        return {'testable': False, 'reason': 'Insufficient data points'}

    if max_lag is None:
        max_lag = max(1, int(n ** (1/3)))  # Common rule of thumb

    mean = statistics.mean(values)
    demeaned = [v - mean for v in values]

    # Gamma_0 = variance
    gamma_0 = sum(d ** 2 for d in demeaned) / n

    # Autocovariances and Newey-West kernel weights
    nw_var = gamma_0
    for lag in range(1, max_lag + 1):
        gamma_lag = sum(demeaned[t] * demeaned[t - lag] for t in range(lag, n)) / n
        weight = 1.0 - lag / (max_lag + 1)  # Bartlett kernel
        nw_var += 2 * weight * gamma_lag

    nw_se = math.sqrt(max(0, nw_var) / n)
    naive_se = math.sqrt(gamma_0 / n)

    # t-statistic with NW standard errors
    t_stat = mean / nw_se if nw_se > 0 else float('inf')
    from math import erf
    p_value = 2 * (1 - 0.5 * (1 + erf(abs(t_stat) / math.sqrt(2))))

    return {
        'testable': True,
        'n': n,
        'mean': mean,
        'max_lag': max_lag,
        'naive_se': naive_se,
        'newey_west_se': nw_se,
        'se_inflation_ratio': nw_se / naive_se if naive_se > 0 else float('inf'),
        't_statistic_nw': t_stat,
        'p_value_two_sided': p_value,
        'ci_95': [mean - 1.96 * nw_se, mean + 1.96 * nw_se],
    }


def time_series_analysis(fills: List[dict]) -> dict:
    """
    Aggregate fills into time bins and compute HAC standard errors.
    Uses 5-minute bins within the trading day.
    """
    # Sort fills by timestamp
    tox_fills = sorted(
        [f for f in fills if f['strategy'] == 'toxicity'],
        key=lambda f: f['fill_time_ns']
    )
    base_fills = sorted(
        [f for f in fills if f['strategy'] == 'baseline'],
        key=lambda f: f['fill_time_ns']
    )

    if not tox_fills:
        return {'testable': False, 'reason': 'No toxicity fills'}

    # Find time range
    min_t = tox_fills[0]['fill_time_ns']
    max_t = tox_fills[-1]['fill_time_ns']

    # 5-minute bins
    bin_size_ns = 5 * 60 * 1_000_000_000  # 5 minutes in nanoseconds
    n_bins = max(1, int((max_t - min_t) / bin_size_ns) + 1)

    # Aggregate PnL per bin for toxicity strategy
    tox_bin_pnl = [0.0] * n_bins
    for fill in tox_fills:
        bin_idx = min(n_bins - 1, int((fill['fill_time_ns'] - min_t) / bin_size_ns))
        # P&L contribution: spread capture minus adverse
        pnl = (fill['fill_price'] - fill['mid_price_at_fill']) * fill['fill_qty']
        if fill['is_buy']:
            pnl = -pnl  # Bought at fill_price, value at mid
        tox_bin_pnl[bin_idx] += pnl + fill['adverse_pnl']

    # Filter out empty bins
    nonempty_bins = [p for p in tox_bin_pnl if p != 0.0]

    if len(nonempty_bins) < 5:
        return {'testable': False, 'reason': f'Only {len(nonempty_bins)} non-empty time bins'}

    nw = newey_west_se(nonempty_bins)
    nw['n_time_bins'] = len(nonempty_bins)
    nw['bin_size_minutes'] = 5
    return nw


# ============================================================================
# Main
# ============================================================================

def main():
    output_dir = None
    for i, arg in enumerate(sys.argv[1:], 1):
        if arg == "--output-dir" and i < len(sys.argv) - 1:
            output_dir = sys.argv[i + 1]

    if not output_dir:
        print("Usage: analyze_fills.py --output-dir <dir_with_csvs>", file=sys.stderr)
        sys.exit(1)

    if not os.path.isdir(output_dir):
        print(f"Directory not found: {output_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Loading data from {output_dir}...")
    fills = load_fills(output_dir)
    symbols = load_symbols(output_dir)

    print(f"  {len(fills)} fills loaded")
    print(f"  {len(symbols)} symbol records loaded")

    tox_fills = [f for f in fills if f['strategy'] == 'toxicity']
    base_fills = [f for f in fills if f['strategy'] == 'baseline']
    print(f"  Toxicity fills: {len(tox_fills)}, Baseline fills: {len(base_fills)}")

    results = {}

    # 1. Toxicity Decile Analysis
    print("\n=== TOXICITY SCORE VALIDATION ===")
    decile = toxicity_decile_analysis(fills)
    results['toxicity_decile_analysis'] = decile
    if decile['testable']:
        print(f"  Fills analyzed: {decile['n_fills_analyzed']}")
        print(f"  {'Decile':<8} {'N':>6} {'Avg Tox':>10} {'Avg Adverse':>14}")
        print(f"  {'-'*42}")
        for d in decile['deciles']:
            print(f"  {d['decile']:<8} {d['n_fills']:>6} {d['avg_toxicity']:>10.4f} "
                  f"${d['avg_adverse_pnl']:>12.6f}")

    # 2. Spearman Rank Correlation
    print("\n=== SPEARMAN RANK CORRELATION (Toxicity vs Adverse) ===")
    spearman = spearman_rank_correlation(tox_fills)
    results['spearman_correlation'] = spearman
    if spearman['testable']:
        print(f"  rho = {spearman['spearman_rho']:.4f}")
        print(f"  n = {spearman['n_fills']}")
        print(f"  p-value = {spearman['p_value_two_sided']:.6f}")
        print(f"  Significant at 0.05: {spearman['significant_at_0.05']}")

    # 2b. Per-Feature Correlation Analysis
    print("\n=== PER-FEATURE CORRELATION (Feature vs Adverse) ===")
    feat_corr = per_feature_correlation(tox_fills)
    results['per_feature_correlation'] = feat_corr
    if feat_corr['testable']:
        print(f"  Fills analyzed: {feat_corr['n_fills']}")
        print(f"  {'Feature':<24} {'rho':>8} {'p-value':>12} {'Sig?':>6} {'Mean':>10} {'Std':>10}")
        print(f"  {'-'*74}")
        for fr in feat_corr['features']:
            sig = '*' if fr['significant'] else ''
            print(f"  {fr['feature']:<24} {fr['spearman_rho']:>8.4f} {fr['p_value']:>12.6f} "
                  f"{sig:>6} {fr['mean_value']:>10.6f} {fr['std_value']:>10.6f}")
    else:
        print(f"  Skipped: {feat_corr.get('reason', 'unknown')}")

    # 3. Symbol-Level Bootstrap
    print("\n=== SYMBOL-LEVEL BOOTSTRAP ===")
    boot = symbol_bootstrap(symbols)
    results['symbol_bootstrap'] = boot
    if boot['testable']:
        print(f"  Active symbols: {boot['n_symbols']}")
        print(f"  Fraction improved: {boot['fraction_symbols_improved']:.4f}")
        print(f"  Mean improvement per symbol: ${boot['observed_mean_improvement']:.4f}")
        ci = boot['bootstrap_ci_95_improvement']
        print(f"  95% CI for mean improvement: [${ci[0]:.4f}, ${ci[1]:.4f}]")
        print(f"  P-value (improvement > 0): {boot['p_value_improvement_gt_zero']:.6f}")
        ci_tox = boot['bootstrap_ci_95_toxicity_pnl']
        print(f"  95% CI for total toxicity PnL: [${ci_tox[0]:.2f}, ${ci_tox[1]:.2f}]")

    # 4. HAC Standard Errors
    print("\n=== HAC (NEWEY-WEST) STANDARD ERRORS ===")
    hac = time_series_analysis(fills)
    results['hac_standard_errors'] = hac
    if hac.get('testable', False):
        print(f"  Time bins (5-min): {hac.get('n_time_bins', 'N/A')}")
        print(f"  Naive SE: ${hac['naive_se']:.4f}")
        print(f"  Newey-West SE: ${hac['newey_west_se']:.4f}")
        print(f"  SE inflation ratio: {hac['se_inflation_ratio']:.2f}x")
        print(f"  t-statistic (NW): {hac['t_statistic_nw']:.4f}")
        print(f"  p-value: {hac['p_value_two_sided']:.6f}")
        ci = hac['ci_95']
        print(f"  95% CI: [${ci[0]:.4f}, ${ci[1]:.4f}]")

    # Save all results
    results_path = os.path.join(output_dir, "advanced_analysis.json")
    with open(results_path, 'w') as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to {results_path}")


if __name__ == "__main__":
    main()
