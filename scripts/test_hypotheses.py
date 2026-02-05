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


def parse_results(path: str) -> Tuple[List[float], List[float], dict]:
    baseline = []
    toxicity = []
    extras = {}
    with open(path, 'r') as f:
        txt = f.read()

    # Find per-group aggregation lines with baseline/toxicity
    pattern = re.compile(r'Aggregation done: [^\n]*baseline \$([\-\d\.]+), toxicity \$([\-\d\.]+)')
    for m in pattern.finditer(txt):
        b = float(m.group(1))
        t = float(m.group(2))
        baseline.append(b)
        toxicity.append(t)

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

    return baseline, toxicity, extras


def sharpe(xs: List[float]) -> float:
    import statistics
    if len(xs) < 2:
        return float('nan')
    mu = statistics.mean(xs)
    sd = statistics.pstdev(xs)
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


def main():
    if len(sys.argv) < 2:
        print('Usage: test_hypotheses.py path/to/results.txt')
        sys.exit(1)
    path = sys.argv[1]
    baseline, toxicity, extras = parse_results(path)

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
    # Not testable per-security/time-series from this file unless per-group adverse metrics exist.
    out['tests']['Adverse Selection Reduction'] = {
        'testable': False,
        'reason': 'No per-group or per-fill adverse-selection time series available in results.txt'
    }

    # Hypothesis 3: Inventory variance reduction
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
    print('Hypothesis tests summary')
    print('Groups parsed:', n)
    print('- Sharpe improvement: obs_diff={:.4f}, p_one_sided={:.4g}, reject@0.05={}'.format(
        out['tests']['Sharpe Improvement']['obs_diff'],
        out['tests']['Sharpe Improvement']['p_value_one_sided'],
        out['tests']['Sharpe Improvement']['reject_at_0.05']))

    print('- Cross-sectional (groups): successes={}/{}, frac={:.3f}, p_one_sided_vs_0.8={:.4g}, pass>0.8={}'.format(
        successes, n, frac, out['tests']['Cross-Sectional Robustness']['p_value_one_sided_vs_0.8'],
        out['tests']['Cross-Sectional Robustness']['pass_threshold_0.8']))

    print('- Monte Carlo (groups as reps): frac={:.3f}, p_one_sided_vs_0.9={:.4g}, pass>=0.9={}'.format(
        frac, out['tests']['Monte Carlo Dominance']['p_value_one_sided_vs_0.9'],
        out['tests']['Monte Carlo Dominance']['pass_threshold_0.9']))

    print('\nNotes:')
    print(' - Hypotheses 2 and 3 require per-fill or per-security time series not present in results.txt;')
    print('   provide detailed per-symbol or per-fill logs to test these formally.')
    print('\nFull JSON report written to', report_path)


if __name__ == '__main__':
    main()
