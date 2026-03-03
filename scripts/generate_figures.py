#!/usr/bin/env python3
"""Generate publication-quality figures for the market-making manuscript.

Produces 4 PNG figures from EV22 simulation results:
  1. cumulative_pnl.png      — Intraday P&L (baseline + 3 filters)
  2. gamma_sensitivity.png    — Inventory risk penalty sensitivity
  3. ablation.png             — Single-component ablation
  4. symbol_improvement.png   — Per-symbol improvement CDF

Usage:
    python scripts/generate_figures.py
"""

import glob
import os
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import numpy as np
import pandas as pd
import seaborn as sns

sns.set_theme(style="whitegrid", context="paper", font_scale=1.1,
              rc={"figure.dpi": 300, "savefig.dpi": 300,
                  "savefig.bbox": "tight", "savefig.pad_inches": 0.08,
                  "axes.edgecolor": ".3", "grid.color": ".88"})

# Colorblind-safe palette (Okabe-Ito)
PAL = sns.color_palette([
    "#0072B2",  # blue  — Walk-Forward
    "#009E73",  # green — SGD Logistic
    "#D55E00",  # vermillion — EWMA
    "#666666",  # grey  — Baseline
    "#56B4E9",  # light blue
    "#E69F00",  # amber
])
C_WF, C_SGD, C_EWMA, C_BASE, C_LIGHT, C_AMBER = PAL

OUT_DIR = "documentation/images"
os.makedirs(OUT_DIR, exist_ok=True)

MARKET_OPEN_NS = 1692708600_000_000_000  # 2023-08-22 13:30 UTC
MAKER_REBATE = 0.0025
TAKER_FEE = 0.003


# ── Data loading ─────────────────────────────────────────────────────────────

def load_fills(result_dir):
    paths = sorted(glob.glob(os.path.join(result_dir, "fills_group_*.csv")))
    frames = [pd.read_csv(p) for p in paths if os.path.getsize(p) > 200]
    return pd.concat(frames, ignore_index=True)


def load_symbols(result_dir):
    paths = sorted(glob.glob(os.path.join(result_dir, "symbols_group_*.csv")))
    return pd.concat([pd.read_csv(p) for p in paths], ignore_index=True)


def compute_cumulative_pnl(fills, strategy):
    df = fills[fills["strategy"] == strategy].sort_values("fill_time_ns")
    if len(df) == 0:
        return np.array([]), np.array([])

    times_ns = df["fill_time_ns"].values
    prices = df["fill_price"].values
    qtys = df["fill_qty"].values.astype(int)
    is_buys = df["is_buy"].values.astype(bool)
    mids = df["mid_price_at_fill"].values
    adverses = df["adverse_pnl"].values
    symbols = df["symbol"].values

    inv = defaultdict(int)
    cb = defaultdict(float)
    last_mid = defaultdict(float)
    sym_unrealized = defaultdict(float)
    realized = fees = adverse_total = 0.0

    n = len(df)
    cum_pnl = np.empty(n)

    for i in range(n):
        sym, price, qty = symbols[i], prices[i], qtys[i]
        is_buy, mid, adv = is_buys[i], mids[i], adverses[i]

        old_mid = last_mid[sym]
        last_mid[sym] = mid

        pos = inv[sym]
        if pos != 0 and old_mid != 0:
            sym_unrealized[sym] = (mid - cb[sym] / abs(pos)) * pos

        adverse_total += abs(adv)
        if abs(mid - price) < 0.02:
            fees += MAKER_REBATE * qty
        else:
            fees -= TAKER_FEE * qty

        signed_qty = qty if is_buy else -qty
        old_inv, old_cb = inv[sym], cb[sym]

        if old_inv == 0:
            inv[sym] = signed_qty
            cb[sym] = price * qty
        elif (old_inv > 0) == is_buy:
            inv[sym] = old_inv + signed_qty
            cb[sym] = old_cb + price * qty
        else:
            close_qty = min(qty, abs(old_inv))
            avg_entry = old_cb / abs(old_inv)
            if old_inv > 0:
                realized += (price - avg_entry) * close_qty
            else:
                realized += (avg_entry - price) * close_qty
            new_inv = old_inv + signed_qty
            remainder = qty - close_qty
            if new_inv == 0:
                cb[sym] = 0.0
            elif abs(new_inv) < abs(old_inv):
                cb[sym] = old_cb * (abs(new_inv) / abs(old_inv))
            else:
                cb[sym] = price * remainder
            inv[sym] = new_inv

        pos = inv[sym]
        sym_unrealized[sym] = ((mid - cb[sym] / abs(pos)) * pos) if pos != 0 else 0.0
        cum_pnl[i] = realized + sum(sym_unrealized.values()) + fees - adverse_total

    hours = (times_ns - MARKET_OPEN_NS) / 3.6e12
    return hours, cum_pnl


def smooth(y, window=50):
    if len(y) < window:
        return y
    k = np.ones(window) / window
    s = np.convolve(y, k, mode="same")
    s[:window // 2] = y[:window // 2]
    s[-window // 2:] = y[-window // 2:]
    return s


def downsample(x, y, n_points=4000):
    if len(x) <= n_points:
        return x, y
    idx = np.linspace(0, len(x) - 1, n_points, dtype=int)
    return x[idx], y[idx]


def save(fig, name):
    path = os.path.join(OUT_DIR, f"{name}.png")
    fig.savefig(path)
    print(f"  -> {path}")


# ── Figure 1: Cumulative P&L ────────────────────────────────────────────────

def figure_cumulative_pnl():
    print("Fig 1: cumulative_pnl")

    fig, (ax_top, ax_bot) = plt.subplots(
        2, 1, figsize=(7.5, 5), height_ratios=[1, 1.5], sharex=True
    )

    # Top panel: baseline
    fills_sgd = load_fills("results/logistic_ev22")
    t_b, pnl_b = compute_cumulative_pnl(fills_sgd, "baseline")
    t_b, pnl_b = downsample(t_b, smooth(pnl_b, 100), 3000)
    ax_top.plot(t_b, pnl_b / 1e3, color=C_BASE, linewidth=1.2)
    ax_top.axhline(0, color="k", linewidth=0.4, linestyle=":")
    ax_top.set_ylabel("P&L ($K)")
    ax_top.legend(["Baseline (no filter)"], loc="lower left", fontsize=8,
                  framealpha=0.9, edgecolor="none")
    ax_top.yaxis.set_major_formatter(
        mticker.FuncFormatter(lambda v, _: f"${v:,.0f}K"))
    ax_top.text(0.98, 0.90, "(a)", transform=ax_top.transAxes,
                fontsize=9, fontweight="bold", va="top", ha="right")
    sns.despine(ax=ax_top)

    # Bottom panel: 3 toxicity filters
    variants = [
        ("results/wf_ev22", "Walk-Forward", C_WF, "-"),
        ("results/logistic_ev22", "SGD Logistic", C_SGD, "--"),
        ("results/ewma_ev22", "EWMA", C_EWMA, "-."),
    ]
    final_pnls = {}
    for result_dir, label, color, ls in variants:
        fills = load_fills(result_dir)
        t, pnl = compute_cumulative_pnl(fills, "toxicity")
        final_pnls[label] = pnl[-1]
        t, pnl = downsample(t, smooth(pnl, 30), 3000)
        ax_bot.plot(t, pnl, color=color, linewidth=1.3, linestyle=ls, label=label)

    ax_bot.axhline(0, color="k", linewidth=0.4, linestyle=":")
    ax_bot.set_xlabel("Hours since market open (9:30 ET)")
    ax_bot.set_ylabel("P&L ($)")
    ax_bot.set_xlim(0, 6.5)

    handles, labels = ax_bot.get_legend_handles_labels()
    new_labels = [f"{l} (+${final_pnls[l]:,.0f})" for l in labels]
    ax_bot.legend(handles, new_labels, loc="upper left", fontsize=8,
                  framealpha=0.9, edgecolor="none")
    ax_bot.yaxis.set_major_formatter(
        mticker.FuncFormatter(lambda v, _: f"${v:,.0f}"))
    ax_bot.text(0.98, 0.90, "(b)", transform=ax_bot.transAxes,
                fontsize=9, fontweight="bold", va="top", ha="right")
    sns.despine(ax=ax_bot)

    fig.align_ylabels([ax_top, ax_bot])
    fig.tight_layout(h_pad=0.6)
    save(fig, "cumulative_pnl")
    plt.close(fig)


# ── Figure 2: Gamma sensitivity ─────────────────────────────────────────────

def figure_gamma_sensitivity():
    print("Fig 2: gamma_sensitivity")

    gamma_labels = [r"$5 \times 10^{-8}$", r"$1 \times 10^{-7}$",
                    r"$2 \times 10^{-7}$"]
    pnls = np.array([-32896, -4490, 1266])
    fills_n = [6206, 3194, 2223]

    colors = [C_EWMA if p < 0 else C_WF for p in pnls]

    fig, ax = plt.subplots(figsize=(5, 3.2))
    x = np.arange(len(gamma_labels))
    bars = ax.bar(x, pnls / 1e3, color=colors, width=0.6, edgecolor="white",
                  linewidth=0.6, zorder=3)

    ax.set_xticks(x)
    ax.set_xticklabels(gamma_labels)
    ax.axhline(0, color="k", linewidth=0.5)
    ax.set_ylabel("Total P&L ($K)")
    ax.set_xlabel(r"Inventory risk penalty $\gamma$")
    ax.yaxis.set_major_formatter(
        mticker.FuncFormatter(lambda v, _: f"${v:,.0f}K"))

    for bar, pnl, nf in zip(bars, pnls, fills_n):
        cx = bar.get_x() + bar.get_width() / 2
        sign = "+" if pnl >= 0 else ""
        if pnl < -10000:
            ax.text(cx, bar.get_y() + bar.get_height() / 2,
                    f"{sign}${pnl:,}\n{nf:,} fills",
                    ha="center", va="center", fontsize=7.5, color="white",
                    fontweight="bold")
        elif pnl < 0:
            ax.text(cx, bar.get_height() - 1.5,
                    f"{sign}${pnl:,}\n{nf:,} fills",
                    ha="center", va="top", fontsize=7.5, fontweight="bold")
        else:
            ax.text(cx, bar.get_height() + 0.5,
                    f"{sign}${pnl:,}  ({nf:,} fills)",
                    ha="center", va="bottom", fontsize=7.5, fontweight="bold")

    sns.despine(ax=ax)
    fig.tight_layout()
    save(fig, "gamma_sensitivity")
    plt.close(fig)


# ── Figure 3: Ablation ──────────────────────────────────────────────────────

def figure_ablation():
    print("Fig 3: ablation")

    spread_pnl = load_symbols("results/ablation_spread")["toxicity_pnl"].sum()
    pnl_filter_pnl = load_symbols("results/ablation_pnl")["toxicity_pnl"].sum()
    obi_pnl = load_symbols("results/ablation_obi")["toxicity_pnl"].sum()
    full_pnl = load_symbols("results/logistic_ev22")["toxicity_pnl"].sum()
    baseline_pnl = load_symbols("results/logistic_ev22")["baseline_pnl"].sum()

    labels = ["Full strategy", "E[PnL] filter only", "Spread widening only",
              "OBI adjustment only", "Baseline (no filter)"]
    pnls = np.array([full_pnl, pnl_filter_pnl, spread_pnl, obi_pnl, baseline_pnl])
    colors = [C_SGD, C_WF, C_LIGHT, C_AMBER, C_BASE]

    fig, ax = plt.subplots(figsize=(7.5, 2.8))
    y = np.arange(len(labels))
    bars = ax.barh(y, pnls / 1e3, color=colors, height=0.6,
                   edgecolor="white", linewidth=0.6, zorder=3)

    for bar, pnl in zip(bars, pnls):
        w = bar.get_width()
        cy = bar.get_y() + bar.get_height() / 2
        sign = "+" if pnl >= 0 else ""
        if pnl >= 0:
            ax.text(w + 4, cy, f"{sign}${pnl:,.0f}",
                    ha="left", va="center", fontsize=8, fontweight="bold")
        else:
            ax.text(-8, cy, f"${pnl:,.0f}",
                    ha="right", va="center", fontsize=8, fontweight="bold",
                    color="white")

    ax.set_yticks(y)
    ax.set_yticklabels(labels, fontsize=9)
    ax.axvline(0, color="k", linewidth=0.5)
    ax.set_xlabel("Total P&L ($K)")
    ax.xaxis.set_major_formatter(
        mticker.FuncFormatter(lambda v, _: f"${v:,.0f}K"))
    ax.invert_yaxis()
    sns.despine(ax=ax, left=True)

    fig.tight_layout()
    save(fig, "ablation")
    plt.close(fig)


# ── Figure 4: Per-symbol improvement CDF ─────────────────────────────────────

def figure_symbol_cdf():
    print("Fig 4: symbol_improvement")

    syms = load_symbols("results/logistic_ev22")
    active = syms[syms["baseline_fills"] > 0].copy()
    active["improvement"] = active["toxicity_pnl"] - active["baseline_pnl"]
    improvements = np.sort(active["improvement"].values)
    cdf = np.arange(1, len(improvements) + 1) / len(improvements)

    frac_improve = np.mean(improvements > 0)
    n_active = len(improvements)

    fig, ax = plt.subplots(figsize=(5, 3.5))
    ax.plot(improvements, cdf, color=C_WF, linewidth=1.3)

    # Shade improvement region
    mask = improvements >= 0
    ax.fill_between(improvements[mask], 0, cdf[mask],
                    alpha=0.12, color=C_WF, zorder=1)

    # Zero crossing
    zero_idx = np.searchsorted(improvements, 0)
    zero_cdf = zero_idx / len(improvements)
    ax.axvline(0, color="k", linewidth=0.4, linestyle=":", alpha=0.5)
    ax.plot(0, zero_cdf, "o", color=C_EWMA, markersize=5, zorder=5)
    ax.annotate(
        f"{frac_improve:.1%} of {n_active:,} symbols\nimprove over baseline",
        xy=(0, zero_cdf), xytext=(0.45, 0.22), textcoords="axes fraction",
        fontsize=8, color=C_EWMA,
        arrowprops=dict(arrowstyle="->", color=C_EWMA, lw=0.8),
    )

    ax.set_xlabel("Per-symbol P&L improvement ($)")
    ax.set_ylabel("Cumulative fraction")
    ax.set_ylim(0, 1.02)

    p1, p99 = np.percentile(improvements, [0.5, 99.5])
    margin = (p99 - p1) * 0.05
    ax.set_xlim(p1 - margin, p99 + margin)

    sns.despine(ax=ax)
    fig.tight_layout()
    save(fig, "symbol_improvement")
    plt.close(fig)


# ── Main ─────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    print(f"Output: {OUT_DIR}/")
    figure_cumulative_pnl()
    figure_gamma_sensitivity()
    figure_ablation()
    figure_symbol_cdf()
    print("Done — 4 PNGs")
