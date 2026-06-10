#!/usr/bin/env python3
"""
plot_bench_cycles_fpga.py -- landscape, big-font bar chart of T1 per-instruction
cycles from a benchmark_instructions_perf run on the FPGA (KV260).

Companion to plot_bench_cycles.py, rendered for a report/slide: wide landscape
figure, large fonts, minimal whitespace, vertical grouped H-vs-V bars
(instruction on the x-axis).

Input is the CSV the FPGA harness writes via `--out` (the canonical perf
artifact, same as run_perf.sh's /tmp/*_perf.csv):

    tag,mode,instruction,iters,total_cycles,cyc_per_iter
    1,H,vadd.vv,100,12345,123.450
    ...

`cyc_per_iter` is the per-issue hardware cycle count (t1_perf_start/stop sum
over `iters` issues, divided by iters). For convenience the harness also prints
[PERF] counter START/STOP lines on stdout, so this script also accepts a
captured stdout log as a fallback (auto-detected).

Usage:
    plot_bench_cycles_fpga.py results/<ts>/bench.csv
    plot_bench_cycles_fpga.py bench.log --scale 1.8 --annotate --out chart.png
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

# Canonical order / tag layout, shared with plot_bench_cycles.py and the harness.
INSTRUCTION_ORDER = [
    "vadd.vv", "vmul.vv", "vmacc.vv", "vmadd.vv", "vand.vv", "vor.vv",
    "vsll.vi", "vsra.vi", "vmseq.vv", "vmsle.vv", "vmsgt.vx", "vmslt.vv",
    "vmand.mm", "vrgather.vx", "vrgather.vx (diag mask)", "vrgather.vv",
    "vmv.s.x", "vmv.x.s", "vmv.v.v", "vredsum.vs", "vredmax.vs",
    "vslideup.vx 1", "vslideup.vx 100", "vslidedown.vx 1", "vslidedown.vx 100",
    "vslideup.vi 1", "vslidedown.vi 1", "vslideup.vi 31", "vslidedown.vi 31",
    "vle8.v full", "vle8.v masked", "vse8.v full", "vse8.v masked",
    "vlse8.v stride2", "vsse8.v stride2",
]
N = len(INSTRUCTION_ORDER)

START_RE = re.compile(r"\[PERF\] counter (\d+) START at cycle (\d+)")
STOP_RE = re.compile(r"\[PERF\] counter STOP at cycle (\d+)")
ITERS_RE = re.compile(r"sum over (\d+) issues")


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
                vals[name] = {"H": 0.0, "V": 0.0}
                order.append(name)
            vals[name][mode] = float(row["cyc_per_iter"])
    return order, [vals[n]["H"] for n in order], [vals[n]["V"] for n in order], iters


def load_perf_log(path: Path):
    """Fallback: parse captured [PERF] stdout. Returns (names, h, v, iters)."""
    totals: dict[int, int] = {}
    iters = None
    pending = None
    with path.open(errors="replace") as f:
        for line in f:
            mi = ITERS_RE.search(line)
            if mi:
                iters = int(mi.group(1))
            m = START_RE.search(line)
            if m:
                pending = (int(m.group(1)), int(m.group(2)))
                continue
            m = STOP_RE.search(line)
            if m and pending is not None:
                tag, start = pending
                totals[tag] = totals.get(tag, 0) + (int(m.group(1)) - start)
                pending = None
    iters = iters or 1
    names, h, v = [], [], []
    for i, name in enumerate(INSTRUCTION_ORDER):
        hv = totals.get(i + 1, 0) / iters
        vv = totals.get(i + 1 + N, 0) / iters
        if hv or vv:
            names.append(name); h.append(hv); v.append(vv)
    return names, h, v, iters


def is_csv(path: Path) -> bool:
    with path.open(errors="replace") as f:
        for line in f:
            line = line.strip()
            if line:
                return line.lower().startswith("tag,mode,instruction")
    return False


def render(names, hv, vv, iters, out_path, title, scale) -> bool:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("matplotlib/numpy not available; `pip install matplotlib`.",
              file=sys.stderr)
        return False

    if not names:
        print("No data to plot.", file=sys.stderr)
        return False

    # Baseline for the per-bar multiplier: Horizontal vadd.vv (= 1.0x).
    base_i = names.index("vadd.vv") if "vadd.vv" in names else 0
    baseline = hv[base_i] if hv[base_i] > 0 \
        else (max((v for v in hv + vv if v > 0), default=1.0))

    base = 24 * scale
    plt.rcParams.update({
        "font.size": base,
        # Title smaller so its long 2nd line fits inside the axes width instead
        # of overflowing left over the y-axis label; y-label smaller to match.
        "axes.titlesize": base * 0.92,
        "axes.labelsize": base * 0.82,
        "xtick.labelsize": base * 0.78,
        "ytick.labelsize": base * 0.9,
        "legend.fontsize": base * 1.05,
        "axes.linewidth": 1.5 * scale,
    })

    n = len(names)
    # Dense (minimal whitespace) so the fonts stay big relative to the figure
    # when it is scaled into a report. Vertical (90 deg) x-labels pack tightly,
    # which is what lets the figure be this narrow without label collisions.
    width = max(16.0, n * 0.5 * scale)
    height = width * 9.0 / 16.0
    fig, ax = plt.subplots(figsize=(width, height))

    x = np.arange(n)
    bw = 0.44
    bars_h = ax.bar(x - bw / 2, hv, bw, label="Horizontal", color="#3b6ea5")
    bars_v = ax.bar(x + bw / 2, vv, bw, label="Vertical", color="#e07b1a")

    ax.set_xticks(x)
    # Diagonal (45 deg) skew: shorter vertical drop than 90 deg so the bottom
    # margin stays compact; ha="right"+anchor aligns each label's end to its
    # tick. Parallel labels don't collide as long as the group pitch holds.
    ax.set_xticklabels(names, rotation=45, ha="right", va="top",
                       rotation_mode="anchor")
    # Push the labels clear of the bar bases; the gap scales with the font so it
    # never jams into the bars as --scale grows. tight_layout then grows the
    # bottom margin to fit the (now lower) labels.
    ax.tick_params(axis="x", pad=10 * scale, length=6 * scale)
    ax.set_ylabel(f"Cycles per issue (Average over {iters} iterations)")
    ax.set_title(title)
    ax.legend(loc="upper left", framealpha=0.9)
    ax.grid(axis="y", linestyle=":", alpha=0.45)
    ax.margins(x=0.01)

    # Headroom so the vertical multiplier labels clear the tallest bars.
    ymax = max(max(hv), max(vv))
    if ymax > 0:
        ax.set_ylim(top=ymax * 1.18)

    # Per-bar multiplier vs Horizontal vadd.vv (1.0x), rotated vertical so the
    # 70 labels never collide horizontally.
    for bars, vals in ((bars_h, hv), (bars_v, vv)):
        for b, val in zip(bars, vals):
            if val <= 0:
                continue
            ax.annotate(f"{val / baseline:.1f}×",
                        (b.get_x() + b.get_width() / 2, b.get_height()),
                        ha="center", va="bottom", fontsize=base * 0.55,
                        rotation=90, xytext=(0, 4), textcoords="offset points")

    fig.tight_layout(pad=0.5)
    fig.savefig(out_path, dpi=150, bbox_inches="tight", pad_inches=0.08)
    plt.close(fig)
    print(f"Saved landscape chart to {out_path}")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", help="bench.csv (preferred) or a captured run log")
    ap.add_argument("--in", dest="in_opt", help="input path (overrides positional)")
    ap.add_argument("--out", help="output PNG (default: <input>/../bench_cycles_fpga.png)")
    ap.add_argument(
        "--title",
        default="2D Vector Processor - Time Multiplexed Vector Instruction Cycle Performance \n"
                "(dLen=128bits, Total Row Element=128, "
                "Total Column Element=128, Element Size=8bits, 1 Hardware Row Instantiated)")
    ap.add_argument("--scale", type=float, default=1.6,
                    help="font/size multiplier (default 1.6; raise for bigger)")
    args = ap.parse_args()

    inp = args.in_opt or args.input
    if not inp:
        print("error: provide bench.csv (or a run log)", file=sys.stderr)
        return 1
    path = Path(inp).resolve()
    if not path.is_file():
        print(f"error: {path} not found", file=sys.stderr)
        return 1

    if is_csv(path):
        names, hv, vv, iters = load_csv(path)
        print(f"Parsed CSV: {len(names)} instructions, iters={iters}")
    else:
        names, hv, vv, iters = load_perf_log(path)
        print(f"Parsed [PERF] log: {len(names)} instructions, iters={iters}")
    if not names:
        print(f"error: no data parsed from {path}", file=sys.stderr)
        return 1

    out_path = Path(args.out).resolve() if args.out \
        else path.parent / "bench_cycles_fpga.png"
    ok = render(names, hv, vv, iters, out_path, args.title, args.scale)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
