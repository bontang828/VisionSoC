#!/usr/bin/env python3
"""
plot_bench_median_fpga.py -- two compact, big-font bar charts summarising the
Horizontal-vs-Vertical cycle cost of the T1 time-multiplexed vector datapath.

Companion to plot_bench_cycles_fpga.py. Where that script plots every
instruction side by side, this one distils the same bench.csv into two
report/slide figures:

  1. AVERAGE chart -- mean cyc_per_iter across all instructions, one bar per
                      mode. Horizontal is the baseline (1.0x); Vertical is drawn
                      relative to it. The x-factor is annotated on top of each
                      bar.
  2. WORST-CASE chart -- the single instruction with the largest Vertical/
                      Horizontal slowdown (vrgather.vv in this dataset), drawn
                      the same way.

Both charts normalise to the Horizontal bar so the y-axis reads as a relative
"Performance difference" (Horizontal = 1.0x), and the x-axis is the Mode.

Input is the canonical harness CSV (same as plot_bench_cycles_fpga.py):

    tag,mode,instruction,iters,total_cycles,cyc_per_iter
    1,H,vadd.vv,5,23540,4708.000
    ...

Usage:
    plot_bench_median_fpga.py results/<ts>/bench.csv
    plot_bench_median_fpga.py bench.csv --scale 1.8 \
        --out-median median.png --out-worst worst.png
"""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
from pathlib import Path

H_COLOR = "#3b6ea5"   # Horizontal (baseline)
V_COLOR = "#e07b1a"   # Vertical


def load_csv(path: Path):
    """Parse the harness CSV. Returns (names, h_vals, v_vals, iters)."""
    order: list[str] = []
    vals: dict[str, dict[str, float]] = {}
    iters = 1
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            name = row["instruction"]
            mode = row["mode"].strip().upper()
            iters = int(row.get("iters", iters) or iters)
            if name not in vals:
                vals[name] = {}
                order.append(name)
            vals[name][mode] = float(row["cyc_per_iter"])
    names = [n for n in order if "H" in vals[n] and "V" in vals[n]]
    h = [vals[n]["H"] for n in names]
    v = [vals[n]["V"] for n in names]
    return names, h, v, iters


def render_pair(label_h, val_h, label_v, val_v, subtitle, out_path,
                title, scale) -> bool:
    """Render a single 2-bar chart: Horizontal baseline (1.0x) vs Vertical."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("matplotlib/numpy not available; `pip install matplotlib`.",
              file=sys.stderr)
        return False

    if val_h <= 0:
        print("error: Horizontal baseline is zero; cannot normalise.",
              file=sys.stderr)
        return False

    # Normalise both bars to the Horizontal baseline so the y-axis is a relative
    # performance difference and Horizontal is exactly 1.0x.
    norm_h = 1.0
    norm_v = val_v / val_h

    base = 24 * scale
    plt.rcParams.update({
        "font.size": base,
        "axes.titlesize": base * 0.92,
        "axes.labelsize": base * 0.9,
        "xtick.labelsize": base * 0.95,
        "ytick.labelsize": base * 0.9,
        "axes.linewidth": 1.5 * scale,
    })

    width = 9.0 * scale
    height = width * 9.0 / 12.0
    fig, ax = plt.subplots(figsize=(width, height))

    x = np.arange(2)
    bars = ax.bar(x, [norm_h, norm_v], 0.55,
                  color=[H_COLOR, V_COLOR],
                  edgecolor="black", linewidth=1.2 * scale)

    ax.set_xticks(x)
    ax.set_xticklabels([label_h, label_v])
    ax.set_xlabel("Mode")
    ax.set_ylabel("Performance difference")
    # Extra pad so the title sits clear of the subtitle / bar annotations.
    ax.set_title(title, pad=26 * scale)
    ax.grid(axis="y", linestyle=":", alpha=0.45)
    ax.margins(x=0.18)

    # Dashed baseline at 1.0x so "Horizontal = baseline" reads at a glance.
    ax.axhline(1.0, color=H_COLOR, linestyle="--", linewidth=1.4 * scale,
               alpha=0.7, zorder=0)

    ymax = max(norm_h, norm_v)
    ax.set_ylim(0, ymax * 1.28)

    # X-factor on top of each bar (Horizontal = 1.0x baseline).
    for b, mult in zip(bars, (norm_h, norm_v)):
        ax.annotate(f"{mult:.2f}×",
                    (b.get_x() + b.get_width() / 2, b.get_height()),
                    ha="center", va="bottom", fontsize=base * 1.05,
                    fontweight="bold", xytext=(0, 6),
                    textcoords="offset points")

    if subtitle:
        ax.text(0.5, 0.99, subtitle, transform=ax.transAxes,
                ha="center", va="top", fontsize=base * 0.55, alpha=0.75)

    fig.tight_layout(pad=0.6)
    fig.savefig(out_path, dpi=150, bbox_inches="tight", pad_inches=0.1)
    plt.close(fig)
    print(f"Saved chart to {out_path}")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", help="bench.csv")
    ap.add_argument("--in", dest="in_opt", help="input path (overrides positional)")
    ap.add_argument("--out-average", "--out-mean", dest="out_average",
                    help="average chart PNG (default: <input>/../bench_average_fpga.png)")
    ap.add_argument("--out-median", dest="out_median",
                    help="median chart PNG (default: <input>/../bench_median_fpga.png)")
    ap.add_argument("--out-worst",
                    help="worst-case chart PNG (default: <input>/../bench_worst_fpga.png)")
    ap.add_argument("--worst-instr",
                    help="force the worst-case instruction (default: auto = max V/H)")
    ap.add_argument("--scale", type=float, default=1.6,
                    help="font/size multiplier (default 1.6)")
    args = ap.parse_args()

    inp = args.in_opt or args.input
    if not inp:
        print("error: provide bench.csv", file=sys.stderr)
        return 1
    path = Path(inp).resolve()
    if not path.is_file():
        print(f"error: {path} not found", file=sys.stderr)
        return 1

    names, hv, vv, iters = load_csv(path)
    if not names:
        print(f"error: no paired H/V data parsed from {path}", file=sys.stderr)
        return 1
    print(f"Parsed CSV: {len(names)} instructions, iters={iters}")

    # --- Average chart: mean across all instructions, per mode. ---
    avg_h = statistics.mean(hv)
    avg_v = statistics.mean(vv)
    print(f"Average cyc/iter  Horizontal={avg_h:.1f}  Vertical={avg_v:.1f}  "
          f"(Vertical = {avg_v / avg_h:.2f}× Horizontal)")

    out_average = Path(args.out_average).resolve() if args.out_average \
        else path.parent / "bench_average_fpga.png"
    ok = render_pair(
        "Horizontal", avg_h, "Vertical", avg_v,
        subtitle=f"Average over {len(names)} instructions "
                 f"(H={avg_h:.0f}, V={avg_v:.0f} cyc/issue)",
        out_path=out_average,
        title="Average Vector Instruction Performance: Horizontal vs Vertical",
        scale=args.scale)
    if not ok:
        return 1

    # --- Median chart: median across all instructions, per mode. ---
    med_h = statistics.median(hv)
    med_v = statistics.median(vv)
    print(f"Median cyc/iter  Horizontal={med_h:.1f}  Vertical={med_v:.1f}  "
          f"(Vertical = {med_v / med_h:.2f}× Horizontal)")

    out_median = Path(args.out_median).resolve() if args.out_median \
        else path.parent / "bench_median_fpga.png"
    ok = render_pair(
        "Horizontal", med_h, "Vertical", med_v,
        subtitle=f"Median over {len(names)} instructions "
                 f"(H={med_h:.0f}, V={med_v:.0f} cyc/issue)",
        out_path=out_median,
        title="Median Vector Instruction Performance: Horizontal vs Vertical",
        scale=args.scale)
    if not ok:
        return 1

    # --- Worst-case chart: largest Vertical/Horizontal slowdown. ---
    if args.worst_instr:
        if args.worst_instr not in names:
            print(f"error: {args.worst_instr!r} not in CSV", file=sys.stderr)
            return 1
        worst = args.worst_instr
    else:
        worst = max(names, key=lambda n: vv[names.index(n)] / hv[names.index(n)]
                    if hv[names.index(n)] > 0 else 0)
    wi = names.index(worst)
    print(f"Worst-case instruction: {worst}  "
          f"H={hv[wi]:.1f}  V={vv[wi]:.1f}  "
          f"(Vertical = {vv[wi] / hv[wi]:.2f}× Horizontal)")

    out_worst = Path(args.out_worst).resolve() if args.out_worst \
        else path.parent / "bench_worst_fpga.png"
    ok = render_pair(
        "Horizontal", hv[wi], "Vertical", vv[wi],
        subtitle=f"{worst}  (H={hv[wi]:.0f}, V={vv[wi]:.0f} cyc/issue)",
        out_path=out_worst,
        title=f"Worst-Case Instruction ({worst}): Horizontal vs Vertical",
        scale=args.scale)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
