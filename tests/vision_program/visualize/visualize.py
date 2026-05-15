#!/usr/bin/env python3
"""
visualize.py - render the input/output 128x128 grids embedded in a
run.log produced by tests/vision_program/* under run-test.sh.

Each demo C program emits one or more grids in this format:

    [GRID_DUMP_BEGIN] <name>
    <128 space-separated decimal ints>
    ... (128 lines)
    [GRID_DUMP_END] <name>

This script parses every such block, renders each as a grayscale image
(int8 range -128..127 -> 0..255 mapped via vmin/vmax), and saves them
side-by-side as a PNG. Optionally also displays the figure.

Usage:

    python3 visualize.py <path/to/run.log>
    python3 visualize.py <path/to/run.log> --out figures/sobel.png
    python3 visualize.py <path/to/run.log> --show

Default output: viz.png next to the input run.log.

Dependencies: numpy, matplotlib.
"""

import argparse
import os
import re
import sys
from pathlib import Path

import numpy as np
import matplotlib

# Default to a non-interactive backend; --show will switch to default.
matplotlib.use("Agg")
import matplotlib.pyplot as plt


GRID_BEGIN = re.compile(r"^\[GRID_DUMP_BEGIN\]\s+(\S+)\s*$")
GRID_END   = re.compile(r"^\[GRID_DUMP_END\]\s+(\S+)\s*$")

ROWS = 128
COLS = 128


def parse_grids(log_text):
    """
    Yield (name, np.ndarray int16 shape (ROWS, COLS)) for each grid
    block found in the given log text.
    """
    grids = []
    lines = log_text.splitlines()
    i = 0
    n = len(lines)
    while i < n:
        m = GRID_BEGIN.match(lines[i])
        if not m:
            i += 1
            continue
        name = m.group(1)
        i += 1
        rows = []
        while i < n:
            mend = GRID_END.match(lines[i])
            if mend:
                if mend.group(1) != name:
                    print(f"[warn] mismatched END tag at line {i}: "
                          f"saw {mend.group(1)!r}, expected {name!r}",
                          file=sys.stderr)
                i += 1
                break
            row = lines[i].strip().split()
            if row:
                try:
                    rows.append([int(x) for x in row])
                except ValueError:
                    # Non-numeric line inside a dump block. Skip silently;
                    # printf interleaving from other code is possible.
                    pass
            i += 1
        if len(rows) == ROWS and all(len(r) == COLS for r in rows):
            grids.append((name, np.array(rows, dtype=np.int16)))
        else:
            actual_rows = len(rows)
            actual_cols = len(rows[0]) if rows else 0
            print(f"[warn] grid {name!r} has unexpected shape "
                  f"{actual_rows}x{actual_cols}, expected {ROWS}x{COLS}; "
                  f"skipping", file=sys.stderr)
    return grids


def render(grids, out_path, show=False):
    n = len(grids)
    if n == 0:
        print("[error] no grids found in run.log", file=sys.stderr)
        return False

    fig_w = max(4, 4 * n)
    fig, axes = plt.subplots(1, n, figsize=(fig_w, 4.5),
                             squeeze=False)
    axes = axes[0]

    for ax, (name, g) in zip(axes, grids):
        # Auto-scale per-grid: int8 ranges from -128..127 in general,
        # but binary {0,1} grids would render as flat gray under that
        # fixed range. Use the grid's own min/max with a small floor
        # to pick a sensible contrast window.
        gmin = int(g.min())
        gmax = int(g.max())
        if gmax - gmin <= 1:
            # Effectively binary or constant - render with 0/1 mapping.
            vmin, vmax = (gmin, gmax) if gmax > gmin else (0, 1)
        else:
            vmin, vmax = -128, 127
        im = ax.imshow(g, cmap="gray", vmin=vmin, vmax=vmax,
                       interpolation="nearest")
        ax.set_title(name, fontsize=10)
        ax.set_xticks([0, COLS // 2, COLS - 1])
        ax.set_yticks([0, ROWS // 2, ROWS - 1])
        plt.colorbar(im, ax=ax, fraction=0.046, pad=0.04)

    fig.suptitle(f"vision_program: {os.path.basename(out_path)}",
                 fontsize=12)
    fig.tight_layout()
    fig.savefig(out_path, dpi=200, bbox_inches="tight")
    print(f"[info] wrote {out_path}")
    if show:
        # Switch backend for interactive display.
        try:
            import matplotlib
            matplotlib.use("TkAgg", force=True)
        except Exception:
            pass
        plt.show()
    plt.close(fig)
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_log", help="path to run.log produced by run-test.sh")
    ap.add_argument("--out", default=None,
                    help="output PNG path (default: viz.png next to run.log)")
    ap.add_argument("--show", action="store_true",
                    help="also display the figure interactively")
    args = ap.parse_args()

    run_log_path = Path(args.run_log)
    if not run_log_path.is_file():
        print(f"[error] {args.run_log}: no such file", file=sys.stderr)
        sys.exit(1)

    out_path = Path(args.out) if args.out else run_log_path.parent / "viz.png"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    text = run_log_path.read_text(errors="replace")
    grids = parse_grids(text)

    if not grids:
        print(f"[error] no [GRID_DUMP_BEGIN] markers found in {args.run_log}",
              file=sys.stderr)
        sys.exit(2)

    print(f"[info] found {len(grids)} grid(s):")
    for name, g in grids:
        print(f"        {name:24s} min={g.min():4d} max={g.max():4d} "
              f"mean={g.mean():.1f}")

    ok = render(grids, str(out_path), show=args.show)
    sys.exit(0 if ok else 3)


if __name__ == "__main__":
    main()
