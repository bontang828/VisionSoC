#!/usr/bin/env python3
"""
plot_bench_cycles.py - Parse a VisionSoC benchmark run.log and render a
grouped bar chart of cycle counts per (instruction, mode).

Usage:
    plot_bench_cycles.py <test-name>           # auto-find newest run.log
    plot_bench_cycles.py --log <path>          # explicit run.log path
    plot_bench_cycles.py <test-name> --out <png-path>

The test-name argument is matched as a substring against the directory
names under test_output/<config>/. For example, "benchmark_instructions"
finds the newest test_output/.../vision_task.benchmark_instructions-*/run.log.

The PNG is saved next to the run.log by default
(run.log/../bench_cycles.png).

Counter-tag layout produced by tests/vision_task/benchmark_instructions:
    1..29   H-mode compute
   30..35   H-mode LSU
   36..64   V-mode compute
   65..70   V-mode LSU
Tags outside this range are still parsed and shown as "tag <N>" with
mode "?", so older benchmarks (e.g. benchmark_vadd) still produce a chart.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
TEST_OUTPUT_ROOT = REPO_ROOT / "test_output"

# Order of bars on the X axis. The C side assigns tags positionally:
# the i-th entry here (1-indexed) is tag i in horizontal mode and tag
# i + len(INSTRUCTION_ORDER) in vertical mode. Adding a new instruction
# means inserting it at the desired slot here AND in the matching slot
# in tests/vision_task/benchmark_instructions/benchmark_instructions.c
# (see N_COMPUTE / N_LSU there). The Python side then derives all tag
# ranges automatically from len(INSTRUCTION_ORDER) - no other constants
# need to be touched.
INSTRUCTION_ORDER = [
    "vadd.vv",
    "vmul.vv",
    "vmacc.vv",
    "vmadd.vv",
    "vand.vv",
    "vor.vv",
    "vsll.vi",
    "vsra.vi",
    "vmseq.vv",
    "vmsle.vv",
    "vmsgt.vx",
    "vmslt.vv",
    "vmand.mm",
    "vrgather.vx",
    "vrgather.vx (diag mask)",
    "vrgather.vv",
    "vmv.s.x",
    "vmv.x.s",
    "vmv.v.v",
    "vredsum.vs",
    "vredmax.vs",
    "vslideup.vx 1",
    "vslideup.vx 100",
    "vslidedown.vx 1",
    "vslidedown.vx 100",
    "vslideup.vi 1",
    "vslidedown.vi 1",
    "vslideup.vi 31",
    "vslidedown.vi 31",
    "vle8.v full",
    "vle8.v masked",
    "vse8.v full",
    "vse8.v masked",
    "vlse8.v stride2",
    "vsse8.v stride2",
]

N_PER_MODE = len(INSTRUCTION_ORDER)

# Tag -> (instruction, mode). H tags are 1..N, V tags are N+1..2N.
TAG_LABELS: dict[int, tuple[str, str]] = {}
for i, name in enumerate(INSTRUCTION_ORDER):
    TAG_LABELS[i + 1] = (name, "H")
    TAG_LABELS[i + 1 + N_PER_MODE] = (name, "V")

# Default number of back-to-back iterations the C kernel runs inside a single
# (counter START, ..., counter STOP) span. Reported cycles are divided by this
# to get per-instruction cost. Override at the CLI with --iters.
# DEFAULT_ITERS = 100
DEFAULT_ITERS = 2

# Tag used as the 1x baseline for the relative-multiplier annotation. Tag 1
# corresponds to "vadd.vv" in horizontal mode by the layout above.
BASELINE_TAG = 1

START_RE = re.compile(r"\[PERF\] counter (\d+) START at cycle (\d+)")
STOP_RE = re.compile(r"\[PERF\] counter STOP at cycle (\d+)")


def find_latest_run_log(test_name: str) -> Path | None:
    """Find the newest run.log under test_output whose dir name contains test_name."""
    if not TEST_OUTPUT_ROOT.is_dir():
        return None
    candidates: list[tuple[float, Path]] = []
    for cfg_dir in TEST_OUTPUT_ROOT.iterdir():
        if not cfg_dir.is_dir():
            continue
        for d in cfg_dir.iterdir():
            if d.is_dir() and test_name in d.name:
                run_log = d / "run.log"
                if run_log.is_file():
                    candidates.append((d.stat().st_mtime, run_log))
    if not candidates:
        return None
    candidates.sort(reverse=True)
    return candidates[0][1]


def parse_log(log_path: Path) -> list[tuple[int, int]]:
    """Return list of (tag, cycles) by pairing each START with the next STOP."""
    pairs: list[tuple[int, int]] = []
    pending: tuple[int, int] | None = None
    with log_path.open("r", errors="replace") as f:
        for line in f:
            m = START_RE.search(line)
            if m:
                # If a START is left dangling (no matching STOP), drop it.
                pending = (int(m.group(1)), int(m.group(2)))
                continue
            m = STOP_RE.search(line)
            if m and pending is not None:
                tag, start = pending
                stop = int(m.group(1))
                pairs.append((tag, stop - start))
                pending = None
    return pairs


def aggregate(pairs: list[tuple[int, int]]) -> dict[int, int]:
    """Sum cycles per tag (one tag may appear in multiple START/STOP spans)."""
    totals: dict[int, int] = {}
    for tag, cycles in pairs:
        totals[tag] = totals.get(tag, 0) + cycles
    return totals


def label_for(tag: int) -> tuple[str, str]:
    return TAG_LABELS.get(tag, (f"tag {tag}", "?"))


def per_iter_cycles(totals: dict[int, int], iters: int) -> dict[int, float]:
    """Average cycles per single instruction execution (total / iters)."""
    return {tag: total / iters for tag, total in totals.items()}


def factor_str(value: float, baseline: float) -> str:
    """Format the multiplier vs baseline. Returns '(--x)' if baseline is 0."""
    if baseline <= 0:
        return "(--x)"
    return f"({value / baseline:.2f}x)"


def print_summary(totals: dict[int, int], iters: int) -> None:
    per_iter = per_iter_cycles(totals, iters)
    baseline = per_iter.get(BASELINE_TAG, 0.0)
    name_b, mode_b = label_for(BASELINE_TAG)
    if baseline > 0:
        print(f"Baseline: tag {BASELINE_TAG} = {name_b} ({mode_b}) "
              f"= {baseline:.2f} cycles/iter -> 1.00x")
    else:
        print(f"Baseline tag {BASELINE_TAG} ({name_b} {mode_b}) "
              f"missing; multipliers reported as --")

    print(f"Found {len(totals)} unique counter tag(s) (iters/span = {iters}):")
    print(f"  {'tag':>4}  {'mode':>4}  {'cycles':>10}  "
          f"{'cyc/iter':>10}  {'vs base':>9}  instruction")
    for tag in sorted(totals.keys()):
        name, mode = label_for(tag)
        cyc = totals[tag]
        per = per_iter[tag]
        print(f"  {tag:>4}  {mode:>4}  {cyc:>10}  "
              f"{per:>10.2f}  {factor_str(per, baseline):>9}  {name}")


def render_text_bar(totals: dict[int, int], iters: int, width: int = 60) -> None:
    """Always-available fallback bar chart drawn with ASCII characters."""
    if not totals:
        return
    per_iter = per_iter_cycles(totals, iters)
    baseline = per_iter.get(BASELINE_TAG, 0.0)
    max_v = max(per_iter.values())
    print()
    print(f"Text bar chart (cycles per iter, max bar width = {width} chars, "
          f"max = {max_v:.1f}):")
    for tag in sorted(totals.keys()):
        name, mode = label_for(tag)
        v = per_iter[tag]
        n = int(round(v * width / max_v)) if max_v > 0 else 0
        fac = factor_str(v, baseline)
        print(f"  [{mode}] {name:<28} | "
              f"{'#' * n}{'.' * (width - n)} {v:.1f} {fac}")


def render_png(totals: dict[int, int], iters: int,
               out_path: Path, title: str) -> bool:
    """Render a grouped H-vs-V bar chart with matplotlib. Returns True on success.

    Bar height is `cycles / iters` (per-instruction cycles). Each bar is
    annotated with that value plus the multiplier vs the baseline tag
    (H vadd by default), e.g. "143.5 / 1.20x".
    """
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available; skipping PNG render. "
              "`pip install matplotlib` to enable.", file=sys.stderr)
        return False

    per_iter = per_iter_cycles(totals, iters)
    baseline = per_iter.get(BASELINE_TAG, 0.0)
    base_name, base_mode = label_for(BASELINE_TAG)

    # Build per-instruction (h, v) pairs in the canonical order. An instruction
    # missing in `totals` (e.g. only one mode was run) becomes 0 for that bar.
    instrs = [name for name in INSTRUCTION_ORDER]
    h_vals: list[float] = []
    v_vals: list[float] = []
    h_tag_for: dict[str, int] = {}
    v_tag_for: dict[str, int] = {}
    for tag, (name, mode) in TAG_LABELS.items():
        if mode == "H":
            h_tag_for[name] = tag
        elif mode == "V":
            v_tag_for[name] = tag
    for name in instrs:
        h_vals.append(per_iter.get(h_tag_for.get(name, -1), 0.0))
        v_vals.append(per_iter.get(v_tag_for.get(name, -1), 0.0))

    # Drop any leading/trailing instruction with both H and V == 0 (i.e. the
    # log doesn't include those tests at all - keeps the chart compact for
    # benchmark_vadd-style runs).
    while instrs and h_vals[0] == 0 and v_vals[0] == 0:
        instrs.pop(0); h_vals.pop(0); v_vals.pop(0)
    while instrs and h_vals[-1] == 0 and v_vals[-1] == 0:
        instrs.pop(); h_vals.pop(); v_vals.pop()

    # Plus any unmapped tags that the log produced anyway (older benchmarks).
    extra_tags = sorted(t for t in totals if t not in TAG_LABELS)
    for t in extra_tags:
        instrs.append(f"tag {t}")
        h_vals.append(per_iter[t])
        v_vals.append(0.0)

    if not instrs:
        print("No data to plot.", file=sys.stderr)
        return False

    n = len(instrs)
    # Top of chart = first instruction. Larger y = lower position, so we
    # plot at -i and rely on invert_yaxis to flip it back, or just set
    # the tick labels in reverse. Simpler: plot at i and invert at the end.
    y = list(range(n))
    bar_h = 0.4

    fig, ax = plt.subplots(figsize=(11, max(6.5, n * 0.45)))
    bars_h = ax.barh([i - bar_h / 2 for i in y], h_vals, bar_h,
                     label="Horizontal", color="steelblue")
    bars_v = ax.barh([i + bar_h / 2 for i in y], v_vals, bar_h,
                     label="Vertical", color="darkorange")

    ax.set_yticks(y)
    ax.set_yticklabels(instrs)
    ax.invert_yaxis()  # first instruction at the top
    ax.set_xlabel(f"Cycles per iteration (total / {iters})")
    if baseline > 0:
        title = f"{title}\nbaseline 1.00x = {base_name} ({base_mode}) " \
                f"= {baseline:.2f} cycles/iter"
    ax.set_title(title)
    ax.legend(loc="lower right")
    ax.grid(axis="x", linestyle=":", alpha=0.5)

    # Headroom on the right so the annotation doesn't run off the axis.
    max_v = max(max(h_vals, default=0.0), max(v_vals, default=0.0))
    if max_v > 0:
        ax.set_xlim(right=max_v * 1.22)

    # Annotate each bar to the right of its tip with per-iter value + factor.
    def annotate(bars: object, vals: list[float]) -> None:
        for bar, v in zip(bars, vals):
            if v <= 0:
                continue
            ax.text(
                v, bar.get_y() + bar.get_height() / 2,
                f" {v:.1f} {factor_str(v, baseline)}",
                ha="left", va="center", fontsize=7,
            )

    annotate(bars_h, h_vals)
    annotate(bars_v, v_vals)

    fig.tight_layout()
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"Saved bar chart to {out_path}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "test", nargs="?",
        help="test-name fragment (e.g. benchmark_instructions); the newest "
             "test_output/<config>/<...test...>-<ts>/run.log is used.",
    )
    parser.add_argument("--log", help="explicit run.log path; overrides <test>")
    parser.add_argument("--out", help="output PNG path; default: same dir as run.log")
    parser.add_argument("--title", help="chart title; default derived from log dir name")
    parser.add_argument("--no-png", action="store_true",
                        help="skip the PNG render (text summary only)")
    parser.add_argument("--iters", type=int, default=DEFAULT_ITERS,
                        help=f"iterations per counter span (default {DEFAULT_ITERS}); "
                             f"reported cycles are divided by this for cyc/iter")
    args = parser.parse_args()
    if args.iters <= 0:
        print("error: --iters must be a positive integer", file=sys.stderr)
        return 1

    if args.log:
        log_path = Path(args.log).resolve()
        if not log_path.is_file():
            print(f"error: {log_path} does not exist", file=sys.stderr)
            return 1
    else:
        if not args.test:
            print("error: provide a test name or --log", file=sys.stderr)
            return 1
        log_path = find_latest_run_log(args.test)
        if log_path is None:
            print(f"error: no run.log matching '{args.test}' under {TEST_OUTPUT_ROOT}",
                  file=sys.stderr)
            return 1
        print(f"Using newest run.log: {log_path}")

    pairs = parse_log(log_path)
    if not pairs:
        print(f"error: no [PERF] counter pairs found in {log_path}", file=sys.stderr)
        return 1

    totals = aggregate(pairs)
    print_summary(totals, args.iters)
    render_text_bar(totals, args.iters)

    if not args.no_png:
        if args.out:
            out_path = Path(args.out).resolve()
        else:
            out_path = log_path.parent / "bench_cycles.png"
        title = args.title or f"VisionSoC bench cycles ({log_path.parent.name})"
        render_png(totals, args.iters, out_path, title)

    return 0


if __name__ == "__main__":
    sys.exit(main())
