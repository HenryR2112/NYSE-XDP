#!/usr/bin/env python3
"""
Parameter Sensitivity Analysis for Market Maker Simulation.

Runs the simulation across a grid of key parameters and collects P&L results.
Generates a sensitivity table showing how profit depends on modeling assumptions.

Usage:
  python3 scripts/parameter_sensitivity.py [--sim-binary build/market_maker_sim]

Key parameters varied:
  - phi (queue_position_fraction): 0.005, 0.01, 0.02, 0.05
  - chi (adverse_selection_multiplier): 0.03, 0.05, 0.10, 0.15
  - mu_latency (latency_us_mean): 5, 10, 20, 50
  - spread_mult (toxicity_spread_multiplier): 0.5, 1.0, 2.0, 4.0
"""
import subprocess
import sys
import os
import re
import json
import itertools
from pathlib import Path


def find_pcap_files(data_dir=None):
    if data_dir is None:
        data_dir = "data/uncompressed-ny4-xnyx-pillar-a-20230822"
    """Find all PCAP files in data directory."""
    pcaps = sorted(Path(data_dir).glob("*.pcap"))
    return [str(p) for p in pcaps]


def run_simulation(sim_binary, pcap_files, symbol_file, params, threads=14,
                   online_learning=False):
    """Run simulation with given parameters and return parsed results."""
    cmd = [sim_binary] + pcap_files + [
        "-s", symbol_file,
        "--threads", str(threads),
        "--queue-fraction", str(params["phi"]),
        "--adverse-multiplier", str(params["chi"]),
        "--latency-us", str(params["latency"]),
        "--toxicity-multiplier", str(params["spread_mult"]),
    ]
    if online_learning:
        cmd.append("--online-learning")

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        output = result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        return None
    except Exception as e:
        print(f"  Error: {e}", file=sys.stderr)
        return None

    # Parse results from output
    parsed = {}

    # Extract baseline and toxicity PnL
    m = re.search(r'Baseline Total PnL: \$([\-\d\.]+)', output)
    if m:
        parsed['baseline_pnl'] = float(m.group(1))

    m = re.search(r'Toxicity Total PnL: \$([\-\d\.]+)', output)
    if m:
        parsed['toxicity_pnl'] = float(m.group(1))

    m = re.search(r'PnL Improvement: \$([\-\d\.]+)', output)
    if m:
        parsed['improvement'] = float(m.group(1))

    m = re.search(r'Toxicity fills: (\d+)', output)
    if m:
        parsed['toxicity_fills'] = int(m.group(1))

    m = re.search(r'Baseline fills: (\d+)', output)
    if m:
        parsed['baseline_fills'] = int(m.group(1))

    m = re.search(r'Quotes suppressed: (\d+)', output)
    if m:
        parsed['quotes_suppressed'] = int(m.group(1))

    # Parse per-group PnL for Sharpe computation
    group_pattern = re.compile(
        r'Group \d+: baseline_pnl=([\-\d\.]+), toxicity_pnl=([\-\d\.]+)'
    )
    group_toxicity_pnls = []
    group_baseline_pnls = []
    for gm in group_pattern.finditer(output):
        group_baseline_pnls.append(float(gm.group(1)))
        group_toxicity_pnls.append(float(gm.group(2)))

    if group_toxicity_pnls:
        import statistics
        mean_t = statistics.mean(group_toxicity_pnls)
        std_t = statistics.stdev(group_toxicity_pnls)
        parsed['sharpe_toxicity'] = mean_t / std_t if std_t > 0 else float('inf')
        mean_b = statistics.mean(group_baseline_pnls)
        std_b = statistics.stdev(group_baseline_pnls)
        parsed['sharpe_baseline'] = mean_b / std_b if std_b > 0 else float('-inf')

    return parsed


def main():
    sim_binary = "build/market_maker_sim"
    symbol_file = "data/symbol_nyse_parsed.csv"
    output_path = "documentation/parameter_sensitivity.json"
    data_dir = None
    online_learning = False

    for arg_idx, arg in enumerate(sys.argv[1:], 1):
        if arg == "--sim-binary" and arg_idx < len(sys.argv) - 1:
            sim_binary = sys.argv[arg_idx + 1]
        elif arg == "--symbol-file" and arg_idx < len(sys.argv) - 1:
            symbol_file = sys.argv[arg_idx + 1]
        elif arg == "--output" and arg_idx < len(sys.argv) - 1:
            output_path = sys.argv[arg_idx + 1]
        elif arg == "--data-dir" and arg_idx < len(sys.argv) - 1:
            data_dir = sys.argv[arg_idx + 1]
        elif arg == "--online-learning":
            online_learning = True

    pcap_files = find_pcap_files(data_dir)
    if not pcap_files:
        print("No PCAP files found", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(pcap_files)} PCAP files")
    print(f"Simulation binary: {sim_binary}")

    # Parameter grid
    param_grid = {
        "phi": [0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5],
        "chi": [0.03, 0.05, 0.10, 0.15],
        "latency": [5, 10, 20, 50, 100, 200, 500],
        "spread_mult": [0.5, 1.0, 2.0, 4.0],
    }

    # Default (baseline) parameters
    defaults = {"phi": 0.005, "chi": 0.03, "latency": 5, "spread_mult": 1.0}

    results = []

    # One-at-a-time sensitivity: vary each parameter while holding others at default
    for param_name, param_values in param_grid.items():
        print(f"\n=== Varying {param_name} ===")
        for val in param_values:
            params = dict(defaults)
            params[param_name] = val
            label = f"{param_name}={val}"
            print(f"  Running {label}...", end="", flush=True)

            parsed = run_simulation(sim_binary, pcap_files, symbol_file, params,
                                   online_learning=online_learning)
            if parsed and 'toxicity_pnl' in parsed:
                print(f" PnL=${parsed['toxicity_pnl']:.2f} "
                      f"(baseline=${parsed.get('baseline_pnl', 0):.2f}, "
                      f"fills={parsed.get('toxicity_fills', 0)})")
                results.append({
                    "varied_param": param_name,
                    "params": params,
                    **parsed,
                })
            else:
                print(" FAILED")

    # Save results
    with open(output_path, 'w') as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to {output_path}")

    # Print summary table
    print("\n" + "=" * 80)
    print("PARAMETER SENSITIVITY SUMMARY")
    print("=" * 80)
    print(f"{'Parameter':<20} {'Value':<10} {'Toxicity PnL':>14} {'Baseline PnL':>14} {'Sharpe':>10} {'Fills':>8}")
    print("-" * 80)
    for r in results:
        param = r['varied_param']
        val = r['params'][param]
        tox_pnl = r.get('toxicity_pnl', 0)
        base_pnl = r.get('baseline_pnl', 0)
        sharpe = r.get('sharpe_toxicity', float('nan'))
        fills = r.get('toxicity_fills', 0)
        marker = " *" if val == defaults[param] else ""
        print(f"{param:<20} {val:<10} ${tox_pnl:>12.2f} ${base_pnl:>12.2f} {sharpe:>10.3f} {fills:>8}{marker}")
    print("-" * 80)
    print("* = default parameter value")


if __name__ == "__main__":
    main()
