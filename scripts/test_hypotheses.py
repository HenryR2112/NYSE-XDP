#!/usr/bin/env python3
"""
Parse simulation results and run hypothesis tests described in the manuscript.

Usage:
  ./scripts/test_hypotheses.py documentation/results.txt

The script extracts per-group `baseline` and `toxicity` PnL values from lines like:
  "... Aggregation done: ... baseline $-138.855, toxicity $38.497"

It runs:
- Hypothesis 1 (Sharpe improvement): bootstrap test on Sharpe(T)-Sharpe(B).
- Hypothesis 4 (Cross-sectional proxy): fraction of groups with PnL_T > PnL_B (>0.8 threshold).
- Hypothesis 5 (Monte Carlo dominance proxy): binomial test vs p0=0.9 using groups as replications.

Hypotheses requiring per-security or time-series inventory/adverse metrics cannot be tested from this file; the script will report which hypotheses are infeasible.
"""
from __future__ import annotations
import sys
import re
import json
import math
import random
from typing import List, Tuple


def parse_results(path: str) -> Tuple[List[float], List[float], dict, dict]:
    baseline = []
    toxicity = []
    extras = {}
    per_group_metrics = {
        'baseline_adverse': [],
        'toxicity_adverse': [],
        'baseline_inv_var': [],
        'toxicity_inv_var': [],
    }
    with open(path, 'r') as f:
        txt = f.read()

    # Find per-group aggregation lines with baseline/toxicity
    pattern = re.compile(r'Aggregation done: [^\n]*baseline \$([\-\d\.]+), toxicity \$([\-\d\.]+)')
    for m in pattern.finditer(txt):
        b = float(m.group(1))
        t = float(m.group(2))
        baseline.append(b)
        toxicity.append(t)

    # Parse new per-group hypothesis testing data format:
    # Group N: baseline_pnl=X, toxicity_pnl=Y, baseline_adv=Z, toxicity_adv=W, baseline_inv_var=A, toxicity_inv_var=B
    group_pattern = re.compile(
        r'Group \d+: baseline_pnl=([\-\d\.]+), toxicity_pnl=([\-\d\.]+), '
        r'baseline_adv=([\-\d\.]+), toxicity_adv=([\-\d\.]+), '
        r'baseline_inv_var=([\-\d\.]+), toxicity_inv_var=([\-\d\.]+)'
    )
    for m in group_pattern.finditer(txt):
        per_group_metrics['baseline_adverse'].append(float(m.group(3)))
        per_group_metrics['toxicity_adverse'].append(float(m.group(4)))
        per_group_metrics['baseline_inv_var'].append(float(m.group(5)))
        per_group_metrics['toxicity_inv_var'].append(float(m.group(6)))

    # Extract some overall metrics if present
    def find_float(label):
        m = re.search(rf'{label}\s*[:=]?\s*\$?([\-\d\.]+)', txt, re.IGNORECASE)
        return float(m.group(1)) if m else None

    extras['Baseline Total PnL'] = find_float('Baseline Total PnL')
    extras['Toxicity Total PnL'] = find_float('Toxicity Total PnL')
    extras['Baseline fills'] = find_float('Baseline fills')
    extras['Toxicity fills'] = find_float('Toxicity fills')
    extras['Adverse Fills'] = find_float('Adverse Fills')
    extras['Total Adverse Penalty'] = find_float('Total Adverse Penalty')

    return baseline, toxicity, extras, per_group_metrics


def sharpe(xs: List[float]) -> float:
    import statistics
    if len(xs) < 2:
        return float('nan')
    mu = statistics.mean(xs)
    sd = statistics.stdev(xs)
    if sd == 0:
        return float('inf') if mu > 0 else float('-inf') if mu < 0 else 0.0
    return mu / sd


def bootstrap_sharpe_diff(baseline: List[float], toxicity: List[float], iters=10000, seed=0):
    random.seed(seed)
    n = len(baseline)
    assert n == len(toxicity) and n > 0
    diffs = []
    for _ in range(iters):
        idxs = [random.randrange(n) for _ in range(n)]
        b_sample = [baseline[i] for i in idxs]
        t_sample = [toxicity[i] for i in idxs]
        diffs.append(sharpe(t_sample) - sharpe(b_sample))
    # p-value for one-sided test H1: Sharpe_T > Sharpe_B
    num_le_zero = sum(1 for d in diffs if d <= 0)
    pval = num_le_zero / len(diffs)
    obs = sharpe(toxicity) - sharpe(baseline)
    return obs, pval, diffs


def binomial_tail_ge(k: int, n: int, p0: float) -> float:
    # P(X >= k) under Binomial(n, p0)
    # compute tail sum
    tail = 0.0
    for i in range(k, n+1):
        tail += math.comb(n, i) * (p0**i) * ((1-p0)**(n-i))
    return tail


def paired_t_test_one_sided(x: List[float], y: List[float]) -> Tuple[float, float]:
    """One-sided paired t-test: H0: mean(x) >= mean(y), H1: mean(x) < mean(y)"""
    import statistics
    if len(x) != len(y) or len(x) < 2:
        return float('nan'), float('nan')
    diffs = [a - b for a, b in zip(x, y)]
    n = len(diffs)
    mean_diff = statistics.mean(diffs)
    std_diff = statistics.stdev(diffs)
    if std_diff == 0:
        return float('inf') if mean_diff < 0 else float('-inf'), 0.0 if mean_diff < 0 else 1.0
    t_stat = mean_diff / (std_diff / math.sqrt(n))
    # One-sided p-value (H1: x < y means mean_diff < 0)
    # Using normal approximation for simplicity
    from math import erf
    def normal_cdf(z):
        return 0.5 * (1 + erf(z / math.sqrt(2)))
    pval = normal_cdf(t_stat)  # P(T < t_stat) for one-sided test
    return t_stat, pval


def main():
    if len(sys.argv) < 2:
        print('Usage: test_hypotheses.py path/to/results.txt')
        sys.exit(1)
    path = sys.argv[1]
    baseline, toxicity, extras, per_group_metrics = parse_results(path)

    out = {'n_groups': len(baseline), 'tests': {}, 'extras': extras}

    if len(baseline) == 0:
        print('No per-group baseline/toxicity entries found in the file. Aborting.')
        sys.exit(2)

    # Hypothesis 1: Sharpe Improvement
    obs_diff, pval_sharpe, _ = bootstrap_sharpe_diff(baseline, toxicity, iters=5000)
    out['tests']['Sharpe Improvement'] = {
        'obs_diff': obs_diff,
        'p_value_one_sided': pval_sharpe,
        'reject_at_0.05': pval_sharpe < 0.05,
        'sharpe_baseline': sharpe(baseline),
        'sharpe_toxicity': sharpe(toxicity),
    }

    # Hypothesis 2: Adverse selection reduction
    # H0: |AS_toxicity| >= |AS_baseline|, H1: |AS_toxicity| < |AS_baseline|
    # Note: adverse values are negative (penalties), so we use absolute values
    # "Better" means less negative (smaller absolute penalty)
    baseline_adv = per_group_metrics['baseline_adverse']
    toxicity_adv = per_group_metrics['toxicity_adverse']
    if len(baseline_adv) >= 2 and len(toxicity_adv) >= 2 and len(baseline_adv) == len(toxicity_adv):
        import statistics
        # Use absolute values since these are penalties (negative numbers)
        baseline_adv_abs = [abs(x) for x in baseline_adv]
        toxicity_adv_abs = [abs(x) for x in toxicity_adv]
        mean_baseline_adv = statistics.mean(baseline_adv_abs)
        mean_toxicity_adv = statistics.mean(toxicity_adv_abs)
        # Test: H1 is that toxicity has LESS adverse (smaller absolute penalty)
        t_stat_adv, pval_adv = paired_t_test_one_sided(toxicity_adv_abs, baseline_adv_abs)
        # Count how many groups have |toxicity_adv| < |baseline_adv| (less adverse = better)
        adv_successes = sum(1 for t, b in zip(toxicity_adv_abs, baseline_adv_abs) if t < b)
        out['tests']['Adverse Selection Reduction'] = {
            'testable': True,
            'n_groups': len(baseline_adv),
            'mean_baseline_adverse_penalty': mean_baseline_adv,
            'mean_toxicity_adverse_penalty': mean_toxicity_adv,
            'reduction_pct': ((mean_baseline_adv - mean_toxicity_adv) / mean_baseline_adv * 100) if mean_baseline_adv != 0 else 0,
            't_statistic': t_stat_adv,
            'p_value_one_sided': pval_adv,
            'reject_at_0.05': pval_adv < 0.05,
            'groups_with_reduction': adv_successes,
            'reduction_fraction': adv_successes / len(baseline_adv),
        }
    else:
        out['tests']['Adverse Selection Reduction'] = {
            'testable': False,
            'reason': 'No per-group or per-fill adverse-selection time series available in results.txt'
        }

    # Hypothesis 3: Inventory variance reduction
    # H0: Var(Q_toxicity) >= Var(Q_baseline), H1: Var(Q_toxicity) < Var(Q_baseline)
    baseline_inv_var = per_group_metrics['baseline_inv_var']
    toxicity_inv_var = per_group_metrics['toxicity_inv_var']
    if len(baseline_inv_var) >= 2 and len(toxicity_inv_var) >= 2 and len(baseline_inv_var) == len(toxicity_inv_var):
        import statistics
        mean_baseline_inv_var = statistics.mean(baseline_inv_var)
        mean_toxicity_inv_var = statistics.mean(toxicity_inv_var)
        t_stat_inv, pval_inv = paired_t_test_one_sided(toxicity_inv_var, baseline_inv_var)
        # Count how many groups have toxicity_inv_var < baseline_inv_var
        inv_successes = sum(1 for t, b in zip(toxicity_inv_var, baseline_inv_var) if t < b)
        out['tests']['Inventory Variance Reduction'] = {
            'testable': True,
            'n_groups': len(baseline_inv_var),
            'mean_baseline_inventory_variance': mean_baseline_inv_var,
            'mean_toxicity_inventory_variance': mean_toxicity_inv_var,
            'reduction_pct': ((mean_baseline_inv_var - mean_toxicity_inv_var) / mean_baseline_inv_var * 100) if mean_baseline_inv_var != 0 else 0,
            't_statistic': t_stat_inv,
            'p_value_one_sided': pval_inv,
            'reject_at_0.05': pval_inv < 0.05,
            'groups_with_reduction': inv_successes,
            'reduction_fraction': inv_successes / len(baseline_inv_var),
        }
    else:
        out['tests']['Inventory Variance Reduction'] = {
            'testable': False,
            'reason': 'No inventory time series per group in results.txt'
        }

    # Hypothesis 4: Cross-Sectional Robustness (proxy using groups)
    successes = sum(1 for b, t in zip(baseline, toxicity) if t > b)
    n = len(baseline)
    frac = successes / n
    # Test H1: frac > 0.8 (one-sided binomial tail under p0=0.8)
    pval_cs = binomial_tail_ge(successes, n, 0.8)
    out['tests']['Cross-Sectional Robustness'] = {
        'n': n,
        'successes': successes,
        'fraction': frac,
        'p_value_one_sided_vs_0.8': pval_cs,
        'pass_threshold_0.8': frac > 0.8
    }

    # Hypothesis 5: Monte Carlo Dominance (proxy using groups as replications)
    # Test H0: true prob p0=0.9 vs H1 p>0.9 using binomial tail
    pval_mc = binomial_tail_ge(successes, n, 0.9)
    out['tests']['Monte Carlo Dominance'] = {
        'n_replications': n,
        'successes': successes,
        'fraction': frac,
        'p_value_one_sided_vs_0.9': pval_mc,
        'pass_threshold_0.9': frac >= 0.9
    }

    # Save JSON report
    report_path = 'documentation/hypothesis_test_results.json'
    with open(report_path, 'w') as jf:
        json.dump(out, jf, indent=2)

    # Print human-readable summary
    print('=' * 60)
    print('HYPOTHESIS TESTS SUMMARY')
    print('=' * 60)
    print('Groups parsed:', n)
    print()

    print('H1 - Sharpe Improvement:')
    print('  obs_diff={:.4f}, p_one_sided={:.4g}, reject@0.05={}'.format(
        out['tests']['Sharpe Improvement']['obs_diff'],
        out['tests']['Sharpe Improvement']['p_value_one_sided'],
        out['tests']['Sharpe Improvement']['reject_at_0.05']))

    print('\nH2 - Adverse Selection Reduction:')
    h2 = out['tests']['Adverse Selection Reduction']
    if h2.get('testable', False):
        print('  mean_baseline_penalty=${:.4f}, mean_toxicity_penalty=${:.4f}'.format(
            h2['mean_baseline_adverse_penalty'], h2['mean_toxicity_adverse_penalty']))
        print('  reduction={:.2f}%, t_stat={:.4f}, p_one_sided={:.4g}, reject@0.05={}'.format(
            h2['reduction_pct'], h2['t_statistic'], h2['p_value_one_sided'], h2['reject_at_0.05']))
        print('  groups_with_reduction={}/{}'.format(h2['groups_with_reduction'], h2['n_groups']))
    else:
        print('  NOT TESTABLE:', h2.get('reason', 'Unknown'))

    print('\nH3 - Inventory Variance Reduction:')
    h3 = out['tests']['Inventory Variance Reduction']
    if h3.get('testable', False):
        print('  mean_baseline_inv_var={:.2f}, mean_toxicity_inv_var={:.2f}'.format(
            h3['mean_baseline_inventory_variance'], h3['mean_toxicity_inventory_variance']))
        print('  reduction={:.2f}%, t_stat={:.4f}, p_one_sided={:.4g}, reject@0.05={}'.format(
            h3['reduction_pct'], h3['t_statistic'], h3['p_value_one_sided'], h3['reject_at_0.05']))
        print('  groups_with_reduction={}/{}'.format(h3['groups_with_reduction'], h3['n_groups']))
    else:
        print('  NOT TESTABLE:', h3.get('reason', 'Unknown'))

    print('\nH4 - Cross-Sectional Robustness:')
    print('  successes={}/{}, frac={:.3f}, p_one_sided_vs_0.8={:.4g}, pass>0.8={}'.format(
        successes, n, frac, out['tests']['Cross-Sectional Robustness']['p_value_one_sided_vs_0.8'],
        out['tests']['Cross-Sectional Robustness']['pass_threshold_0.8']))

    print('\nH5 - Monte Carlo Dominance:')
    print('  frac={:.3f}, p_one_sided_vs_0.9={:.4g}, pass>=0.9={}'.format(
        frac, out['tests']['Monte Carlo Dominance']['p_value_one_sided_vs_0.9'],
        out['tests']['Monte Carlo Dominance']['pass_threshold_0.9']))

    print('\n' + '=' * 60)
    print('Full JSON report written to', report_path)


if __name__ == '__main__':
    main()
