#!/usr/bin/env python3
"""
Market Maker Simulation Results Analyzer

Analyzes the output from market_maker_sim to compute statistical metrics
including Sharpe ratio, fill analysis, and power calculations.

Usage:
    ./market_maker_sim ... | python scripts/analyze_results.py
    # Or pipe from a log file:
    cat simulation_output.log | python scripts/analyze_results.py
"""

import sys
import re
import math
from dataclasses import dataclass, field
from typing import List, Optional
import statistics


@dataclass
class SimulationResults:
    """Parsed simulation results"""
    # Performance metrics
    baseline_pnl: float = 0.0
    toxicity_pnl: float = 0.0
    pnl_improvement: float = 0.0
    improvement_pct: float = 0.0

    # Fill metrics
    baseline_fills: int = 0
    toxicity_fills: int = 0
    quotes_suppressed: int = 0
    adverse_fills: int = 0
    adverse_penalty: float = 0.0

    # Throughput metrics
    total_packets: int = 0
    total_messages: int = 0
    processing_time_sec: float = 0.0
    packets_per_sec: int = 0
    messages_per_sec: int = 0

    # Symbol metrics
    unique_symbols: int = 0
    process_groups: int = 0

    # Per-group results (for variance calculation)
    group_pnls: List[float] = field(default_factory=list)


def parse_simulation_output(text: str) -> SimulationResults:
    """Parse simulation output text into structured results"""
    results = SimulationResults()

    # Parse aggregated results
    if m := re.search(r"Baseline Total PnL:\s*\$?([-\d.]+)", text):
        results.baseline_pnl = float(m.group(1))

    if m := re.search(r"Toxicity Total PnL:\s*\$?([-\d.]+)", text):
        results.toxicity_pnl = float(m.group(1))

    if m := re.search(r"PnL Improvement:\s*\$?([-\d.]+)\s*\(([-\d.]+)%\)", text):
        results.pnl_improvement = float(m.group(1))
        results.improvement_pct = float(m.group(2))

    # Fill metrics
    if m := re.search(r"Baseline fills:\s*(\d+)", text):
        results.baseline_fills = int(m.group(1))

    if m := re.search(r"Toxicity fills:\s*(\d+)", text):
        results.toxicity_fills = int(m.group(1))

    if m := re.search(r"Quotes suppressed:\s*(\d+)", text):
        results.quotes_suppressed = int(m.group(1))

    if m := re.search(r"Adverse fills:\s*(\d+)", text):
        results.adverse_fills = int(m.group(1))

    if m := re.search(r"Total adverse penalty:\s*\$?([-\d.]+)", text):
        results.adverse_penalty = float(m.group(1))

    # Throughput
    if m := re.search(r"Total processing time:\s*([\d.]+)\s*seconds", text):
        results.processing_time_sec = float(m.group(1))

    if m := re.search(r"Total packets:\s*(\d+)", text):
        results.total_packets = int(m.group(1))

    if m := re.search(r"Total messages:\s*(\d+)", text):
        results.total_messages = int(m.group(1))

    if m := re.search(r"Throughput:\s*(\d+)\s*packets/sec.*?(\d+)\s*msgs/sec", text):
        results.packets_per_sec = int(m.group(1))
        results.messages_per_sec = int(m.group(2))

    if m := re.search(r"Unique symbols.*?:\s*(\d+)", text):
        results.unique_symbols = int(m.group(1))

    if m := re.search(r"Process groups:\s*(\d+)", text):
        results.process_groups = int(m.group(1))

    # Extract per-group PnL for variance calculation
    for m in re.finditer(r"toxicity \$?([-\d.]+)", text):
        results.group_pnls.append(float(m.group(1)))

    return results


def calculate_sharpe_ratio(returns: List[float], risk_free_rate: float = 0.0) -> float:
    """
    Calculate Sharpe ratio from a list of returns.

    Sharpe = (mean_return - risk_free_rate) / std_dev_return

    For intraday HFT, risk-free rate is typically 0.
    """
    if len(returns) < 2:
        return 0.0

    mean_return = statistics.mean(returns)
    std_return = statistics.stdev(returns)

    if std_return == 0:
        return float('inf') if mean_return > 0 else 0.0

    return (mean_return - risk_free_rate) / std_return


def calculate_annualized_sharpe(daily_sharpe: float, trading_days: int = 252) -> float:
    """Annualize daily Sharpe ratio"""
    return daily_sharpe * math.sqrt(trading_days)


def calculate_fill_efficiency(results: SimulationResults) -> dict:
    """Calculate fill efficiency metrics"""
    total_quotes = results.toxicity_fills + results.quotes_suppressed

    return {
        "fill_rate": results.toxicity_fills / total_quotes if total_quotes > 0 else 0,
        "suppression_rate": results.quotes_suppressed / total_quotes if total_quotes > 0 else 0,
        "adverse_fill_rate": results.adverse_fills / results.toxicity_fills if results.toxicity_fills > 0 else 0,
        "pnl_per_fill": results.toxicity_pnl / results.toxicity_fills if results.toxicity_fills > 0 else 0,
        "baseline_pnl_per_fill": results.baseline_pnl / results.baseline_fills if results.baseline_fills > 0 else 0,
    }


def calculate_statistical_power(
    observed_effect: float,
    std_dev: float,
    n_observations: int,
    alpha: float = 0.05
) -> float:
    """
    Calculate statistical power for detecting the observed effect.

    Power = P(reject H0 | H1 is true)

    Uses normal approximation for large samples.
    """
    if std_dev == 0 or n_observations == 0:
        return 0.0

    # Standard error
    se = std_dev / math.sqrt(n_observations)

    # Effect size (Cohen's d)
    effect_size = abs(observed_effect) / std_dev if std_dev > 0 else 0

    # Z-score for alpha (two-tailed)
    z_alpha = 1.96 if alpha == 0.05 else 2.576  # 0.05 or 0.01

    # Non-centrality parameter
    ncp = abs(observed_effect) / se

    # Power approximation using normal distribution
    # Power ≈ Φ(|δ|√n - z_α/2) for large n
    power = 0.5 * (1 + math.erf((ncp - z_alpha) / math.sqrt(2)))

    return min(1.0, max(0.0, power))


def calculate_t_statistic(mean: float, std_dev: float, n: int) -> float:
    """Calculate t-statistic for testing if mean differs from 0"""
    if std_dev == 0 or n == 0:
        return 0.0
    se = std_dev / math.sqrt(n)
    return mean / se if se > 0 else 0.0


def estimate_required_sample_size(
    effect_size: float,
    std_dev: float,
    power: float = 0.80,
    alpha: float = 0.05
) -> int:
    """
    Estimate required sample size to detect effect with given power.

    n = (z_α + z_β)² * σ² / δ²
    """
    if effect_size == 0:
        return float('inf')

    # Z-scores
    z_alpha = 1.96  # 0.05 two-tailed
    z_beta = 0.84 if power == 0.80 else 1.28  # 0.80 or 0.90 power

    n = ((z_alpha + z_beta) ** 2 * std_dev ** 2) / (effect_size ** 2)
    return int(math.ceil(n))


def print_analysis(results: SimulationResults):
    """Print comprehensive analysis of simulation results"""

    print("=" * 70)
    print("MARKET MAKER SIMULATION STATISTICAL ANALYSIS")
    print("=" * 70)

    # Basic P&L Summary
    print("\n--- P&L SUMMARY ---")
    print(f"Baseline Strategy PnL:     ${results.baseline_pnl:>12,.2f}")
    print(f"Toxicity Strategy PnL:     ${results.toxicity_pnl:>12,.2f}")
    print(f"Strategy Improvement:      ${results.pnl_improvement:>12,.2f} ({results.improvement_pct:+.2f}%)")

    # Fill Analysis
    print("\n--- FILL ANALYSIS ---")
    fill_metrics = calculate_fill_efficiency(results)
    print(f"Baseline Fills:            {results.baseline_fills:>12,}")
    print(f"Toxicity Fills:            {results.toxicity_fills:>12,}")
    print(f"Quotes Suppressed:         {results.quotes_suppressed:>12,}")
    print(f"Fill Rate:                 {fill_metrics['fill_rate']:>12.2%}")
    print(f"Suppression Rate:          {fill_metrics['suppression_rate']:>12.2%}")
    print(f"Adverse Fill Rate:         {fill_metrics['adverse_fill_rate']:>12.2%}")
    print(f"PnL per Fill (Toxicity):   ${fill_metrics['pnl_per_fill']:>12.4f}")
    print(f"PnL per Fill (Baseline):   ${fill_metrics['baseline_pnl_per_fill']:>12.4f}")

    # Adverse Selection
    print("\n--- ADVERSE SELECTION ---")
    print(f"Adverse Fills:             {results.adverse_fills:>12,}")
    print(f"Total Adverse Penalty:     ${results.adverse_penalty:>12,.2f}")
    if results.adverse_fills > 0:
        avg_adverse = results.adverse_penalty / results.adverse_fills
        print(f"Avg Adverse per Fill:      ${avg_adverse:>12.4f}")

    # Statistical Analysis (if we have per-group data)
    if len(results.group_pnls) >= 2:
        print("\n--- STATISTICAL ANALYSIS ---")
        mean_pnl = statistics.mean(results.group_pnls)
        std_pnl = statistics.stdev(results.group_pnls)
        n_groups = len(results.group_pnls)

        print(f"Number of Groups:          {n_groups:>12}")
        print(f"Mean Group PnL:            ${mean_pnl:>12.2f}")
        print(f"Std Dev Group PnL:         ${std_pnl:>12.2f}")

        # Sharpe Ratio
        sharpe = calculate_sharpe_ratio(results.group_pnls)
        annualized_sharpe = calculate_annualized_sharpe(sharpe)
        print(f"\nIntra-Day Sharpe Ratio:    {sharpe:>12.3f}")
        print(f"Annualized Sharpe (est):   {annualized_sharpe:>12.3f}")

        # T-statistic
        t_stat = calculate_t_statistic(mean_pnl, std_pnl, n_groups)
        print(f"T-Statistic (vs 0):        {t_stat:>12.3f}")

        # Statistical Power
        power = calculate_statistical_power(mean_pnl, std_pnl, n_groups)
        print(f"Statistical Power (α=0.05):{power:>12.3f}")

        # Required sample size for 80% power
        if mean_pnl != 0:
            required_n = estimate_required_sample_size(abs(mean_pnl), std_pnl)
            print(f"Sample Size for 80% Power: {required_n:>12}")

        # Confidence Interval
        se = std_pnl / math.sqrt(n_groups)
        ci_lower = mean_pnl - 1.96 * se
        ci_upper = mean_pnl + 1.96 * se
        print(f"95% CI for Mean PnL:       [${ci_lower:.2f}, ${ci_upper:.2f}]")

    # Throughput Summary
    print("\n--- THROUGHPUT ---")
    print(f"Total Packets:             {results.total_packets:>12,}")
    print(f"Total Messages:            {results.total_messages:>12,}")
    print(f"Processing Time:           {results.processing_time_sec:>12.1f} sec")
    print(f"Packets/sec:               {results.packets_per_sec:>12,}")
    print(f"Messages/sec:              {results.messages_per_sec:>12,}")

    # Scaling Analysis
    print("\n--- SCALING ANALYSIS ---")
    print(f"Process Groups Used:       {results.process_groups:>12}")
    print(f"Unique Symbols (sum):      {results.unique_symbols:>12,}")
    if results.process_groups > 0:
        msgs_per_group = results.total_messages / results.process_groups
        print(f"Msgs per Group:            {msgs_per_group:>12,.0f}")

    # P&L Scaling Projections
    print("\n--- P&L SCALING PROJECTIONS ---")
    print("(Based on linear scaling assumptions)")

    # Current simulation represents ~1 day
    daily_pnl = results.toxicity_pnl

    # Project with different quote sizes (current is 100 shares)
    print(f"\nCurrent Daily PnL (100 share quotes):  ${daily_pnl:>12,.2f}")

    for multiplier, label in [(10, "1,000"), (50, "5,000"), (100, "10,000")]:
        projected = daily_pnl * multiplier
        annual = projected * 252
        print(f"With {label} share quotes:")
        print(f"  Daily:  ${projected:>12,.2f}  |  Annual: ${annual:>12,.2f}")

    print("\n" + "=" * 70)
    print("ANALYSIS CAVEATS:")
    print("=" * 70)
    print("""
1. Linear scaling assumes no market impact - unrealistic for large sizes
2. Queue position modeling may be optimistic or pessimistic
3. Adverse selection measurement uses 500μs lookforward window
4. Fill probability depends heavily on queue position assumptions
5. Real HFT systems have additional costs (infrastructure, data, clearing)
6. Simulation doesn't model inventory risk overnight or hedging costs
7. Results are for ONE day - significant day-to-day variance expected
""")


def main():
    # Read from stdin or file
    if len(sys.argv) > 1:
        with open(sys.argv[1], 'r') as f:
            text = f.read()
    else:
        text = sys.stdin.read()

    if not text.strip():
        print("Error: No input provided")
        print(__doc__)
        sys.exit(1)

    results = parse_simulation_output(text)
    print_analysis(results)


if __name__ == "__main__":
    main()
