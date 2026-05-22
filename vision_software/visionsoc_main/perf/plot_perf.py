#!/usr/bin/env python3
"""
Plot stacked-horizontal-bar breakdowns from the CSVs produced by
visionsoc_main_perf and sobel_perf.

Usage:
    plot_perf.py pipeline pipeline_perf.csv --out pipeline.png
    plot_perf.py sobel    sobel_perf.csv    [--out sobel.png]
                                            [--t1-hz 60_000_000]
                                            [--per-instr | --grouped | --both]

The sobel chart is a single figure with TWO horizontal bars: top = T1
cycles, bottom = wall-clock microseconds. Same colours on both so the
eye can match segments. Narrow segments don't get inline labels; the
side legend always lists every segment with its mean cycles and us.

The pipeline chart is a single bar (wall-microseconds) with the libt1
overhead drawn as a hatched sliver inside the T1 segment.
"""
from __future__ import annotations

import argparse
import csv
import os
import sys
from collections import defaultdict
from dataclasses import dataclass
from typing import Dict, List, Tuple

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
import numpy as np


# -----------------------------------------------------------------------------
# Pipeline (main_perf.c) -- fixed column order matches the CSV header.
# -----------------------------------------------------------------------------

PIPELINE_STAGES: List[str] = [
    "cam_dq_us",
    "stats_in_us",
    "prep_in_us",
    "dma_in_us",
    "t1_kernel_us",
    "dma_out_us",
    "sync_out_us",
    "stats_out_us",
    "uv_fill_us",
    "disp_dq_us",
    "rgb_conv_us",
    "disp_qb_us",
    "cam_qb_us",
]

PIPELINE_LABELS: Dict[str, str] = {
    "cam_dq_us":     "cam_dq",
    "stats_in_us":   "stats_in",
    "prep_in_us":    "memcpy+sync_in",
    "dma_in_us":     "DMA DDR->URAM",
    "t1_kernel_us":  "T1 kernel",
    "dma_out_us":    "DMA URAM->DDR",
    "sync_out_us":   "sync_out",
    "stats_out_us":  "stats_out",
    "uv_fill_us":    "UV fill",
    "disp_dq_us":    "disp_dq",
    "rgb_conv_us":   "RGB convert",
    "disp_qb_us":    "disp_qb",
    "cam_qb_us":     "cam_qb",
}

# Per-stage colour overrides. tab20's slot for the T1 kernel was a
# warning-red that visually screamed "this is a problem stage" -- the
# opposite of the message we want for the accelerator. Replace it with
# a bright warm yellow (Tailwind amber-400) which reads as "premium /
# hero block" against the muted neighbours from tab20.
PIPELINE_COLOR_OVERRIDES: Dict[str, str] = {
    "t1_kernel_us": "#FBBF24",   # bright warm yellow -- hero accelerator
    "rgb_conv_us":  "#E6E6FA",   # pale lavender -- CPU-side colour conv
    "disp_qb_us":   "#ADD8E6",   # pale blue -- DRM vsync wait
}

LABEL_INLINE_MIN_FRAC = 0.05  # only inline-label segments >= this share of total


def _human_us(us: float) -> str:
    # Always render in microseconds with a thousands separator. Mixing
    # "us" and "ms" units in the same chart was hard to compare at a
    # glance; one unit is easier to scan even if the number is longer.
    return f"{us:,.0f}us"


def _human_cyc(c: float) -> str:
    if c >= 1000:
        return f"{c/1000:.1f}k"
    return f"{c:.0f}"


def plot_pipeline(csv_path: str, out_path: str, t1_hz: float,
                  skip_warmup: bool = True) -> None:
    rows = []
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            rows.append(row)
    if not rows:
        sys.exit(f"no rows in {csv_path}")

    # Exclude the first frame -- camera and display ramp-up costs there
    # are several orders of magnitude larger than steady-state and wreck
    # the bar geometry. Note this in the title.
    skipped = 0
    if skip_warmup and len(rows) > 1:
        rows = rows[1:]
        skipped = 1

    means: Dict[str, float] = {}
    stds: Dict[str, float] = {}
    for stage in PIPELINE_STAGES:
        vals = np.array([float(r[stage]) for r in rows])
        means[stage] = float(vals.mean())
        stds[stage] = float(vals.std())

    cycles_mean = np.mean([float(r["t1_kernel_cycles"]) for r in rows])
    t1_pure_us = cycles_mean * 1e6 / t1_hz
    libt1_us = max(0.0, means["t1_kernel_us"] - t1_pure_us)
    total_us = sum(means.values())
    frame_total = float(np.mean([float(r["frame_total_us"]) for r in rows]))

    # constrained_layout handles the right-side legend cleanly without
    # the tight_layout warning we got before.
    fig, ax = plt.subplots(figsize=(15, 3.5), constrained_layout=True)
    palette = list(plt.cm.tab20(np.linspace(0, 1, len(PIPELINE_STAGES))))
    # Apply per-stage colour overrides. Stage order matches
    # PIPELINE_STAGES, so we look up each override by name and patch
    # the matching index in the palette.
    for stage, hex_colour in PIPELINE_COLOR_OVERRIDES.items():
        if stage in PIPELINE_STAGES:
            palette[PIPELINE_STAGES.index(stage)] = hex_colour

    # Column widths used in the legend rows; the header below uses the
    # same widths so the columns line up vertically. The numeric
    # %-column is rendered with one decimal (e.g. "53.6%") = 5 chars.
    LBL_W = 16   # stage label column (fits "-> libt1 sliver" = 15 chars)
    US_W  = 9    # mean us column (e.g. "27,707us")
    PCT_W = 6    # percent column (e.g. " 53.6%")

    def _row(label: str, us_str: str, pct_str: str) -> str:
        return f"{label:<{LBL_W}s} {us_str:>{US_W}s}  {pct_str:>{PCT_W}s}"

    left = 0.0
    stage_boundaries: List[float] = []
    label_thresh = total_us * LABEL_INLINE_MIN_FRAC
    legend_handles: List[Patch] = []

    # Header row first -- invisible patch so the column widths match the
    # data rows but no colour marker is drawn.
    legend_handles.append(Patch(
        facecolor="none", edgecolor="none",
        label=_row("stage", "mean", "%"),
    ))

    for stage, color in zip(PIPELINE_STAGES, palette):
        width = means[stage]
        ax.barh([0], [width], left=left, height=0.55,
                color=color, edgecolor="black", linewidth=0.4)
        if width >= label_thresh:
            # Three lines so the % doesn't bleed past the segment edge
            # when the wall-us number is wide (e.g. "27,707us").
            ax.text(left + width / 2, 0,
                    f"{PIPELINE_LABELS[stage]}\n"
                    f"{_human_us(width)}\n"
                    f"{100*width/total_us:.1f}%",
                    ha="center", va="center", fontsize=9, color="black")
        legend_handles.append(Patch(
            facecolor=color, edgecolor="black",
            label=_row(PIPELINE_LABELS[stage],
                       _human_us(width),
                       f"{100*width/total_us:.1f}%"),
        ))
        left += width
        stage_boundaries.append(left)

    # Hatched libt1 sliver inside the T1 segment.
    t1_left = sum(means[s] for s in PIPELINE_STAGES
                  if PIPELINE_STAGES.index(s) < PIPELINE_STAGES.index("t1_kernel_us"))
    if libt1_us > 0 and means["t1_kernel_us"] > 0:
        ax.barh([0], [libt1_us],
                left=t1_left + t1_pure_us, height=0.55,
                color="none", edgecolor="black",
                hatch="///", linewidth=0.5)
        legend_handles.append(Patch(
            facecolor="white", edgecolor="black", hatch="///",
            label=_row("-> libt1 sliver",
                       _human_us(libt1_us),
                       f"{100*libt1_us/total_us:.2f}%"),
        ))

    # Dotted separators at every internal pipeline-stage boundary. The
    # final boundary is just the bar's right edge, so skip it.
    for x in stage_boundaries[:-1]:
        ax.plot([x, x], [-0.34, 0.34],
                linestyle=":", color="dimgray", linewidth=1.0,
                clip_on=False, zorder=6)

    ax.set_yticks([])
    ax.set_xlabel("microseconds per frame  (mean across frames)")
    sub = (f"(skipping frame 0 warm-up)" if skipped else "")
    # fig.suptitle centres across the full figure width (which includes
    # the right-side legend), so the title appears centred over the
    # combined bar+legend rather than left-aligned over just the bar.
    fig.suptitle(
        f"VisionSoC pipeline per-frame breakdown   "
        f"N={len(rows)} frames {sub}\n"
        f"mean frame = {_human_us(frame_total)}  ~{1e6/frame_total:.1f} fps    "
        f"T1 kernel = {_human_us(means['t1_kernel_us'])} wall "
        f"= {_human_us(t1_pure_us)} T1@{t1_hz/1e6:.0f}MHz "
        f"+ {_human_us(libt1_us)} libt1/Linux",
        fontsize=10,
    )
    ax.set_xlim(0, total_us * 1.02)
    ax.set_ylim(-0.5, 0.5)

    # Right-side, monospace legend so the columns line up. The header
    # row is added to legend_handles above so it gets the same indent as
    # the data rows (matplotlib's legend `title=` parameter would put the
    # header at a different x offset). The header text is bolded by
    # walking leg.get_texts() after the legend is built.
    leg = ax.legend(handles=legend_handles,
                    loc="center left", bbox_to_anchor=(1.01, 0.5),
                    ncol=1, fontsize=8, frameon=False,
                    prop={"family": "monospace"})
    leg.get_texts()[0].set_fontweight("bold")

    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_path}: {len(rows)} frames, total={_human_us(frame_total)}, "
          f"~{1e6/frame_total:.1f} fps")


# -----------------------------------------------------------------------------
# Sobel (sobel_perf.c) -- per-instruction CSV.
# -----------------------------------------------------------------------------

SOBEL_GROUPS: List[Tuple[str, List[int]]] = [
    ("VLE-load (vle8.v)",        [0]),
    ("Gx pass (slides + sub)",   [1, 2, 3, 4, 5]),
    ("H->V CSR flip",            [6]),
    ("Gy pass (slides + sub)",   [7, 8, 9, 10, 11]),
    ("V->H CSR flip",            [12]),
    ("|Gx|+|Gy| (rsub+max+sat)", [13, 14, 15, 16, 17]),
    ("VSE-store (vse8.v)",       [18]),
]

# Short, fixed-width category names used in the per-instr legend.
SOBEL_GROUP_SHORT: Dict[int, str] = {
    0: "VLE-load",
    **{i: "Gx pass"     for i in (1, 2, 3, 4, 5)},
    6: "CSR flip",
    **{i: "Gy pass"     for i in (7, 8, 9, 10, 11)},
    12: "CSR flip",
    **{i: "|Gx|+|Gy|"   for i in (13, 14, 15, 16, 17)},
    18: "VSE-store",
}


# -----------------------------------------------------------------------------
# Optical flow (optical_flow_perf.c) -- 5-direction block-matching kernel
# plus a static/noise deadband. Instruction indices follow the .S layout:
# 50 words total, with 4 dispatcher-only csrwi at indices 22, 28, 32, 38.
# -----------------------------------------------------------------------------

OPTICAL_FLOW_GROUPS: List[Tuple[str, List[int]]] = [
    ("VLE-load curr (a0)",            [0]),
    ("VLE-load prev (a1)",            [1]),
    ("SAD_static + argmin init",      [2, 3, 4, 5]),
    ("SAD_right (vmv+H slideup)",     [6, 7, 8, 9, 10, 11, 12, 13]),
    ("SAD_left (vmv+H slidedown)",    [14, 15, 16, 17, 18, 19, 20, 21]),
    ("H->V CSR flip (down)",          [22]),
    ("V-mode SAD_down (vmv+slide+sub+abs)", [23, 24, 25, 26, 27]),
    ("V->H CSR flip (down)",          [28]),
    ("SAD_down compare+merge",        [29, 30, 31]),
    ("H->V CSR flip (up)",            [32]),
    ("V-mode SAD_up (vmv+slide+sub+abs)",   [33, 34, 35, 36, 37]),
    ("V->H CSR flip (up)",            [38]),
    ("SAD_up compare+merge",          [39, 40, 41]),
    ("Static deadband",               [42, 43, 44, 45, 46]),
    ("Scale argmin x50",              [47]),
    ("VSE motion (a2)",               [48]),
    ("VSE prev<-curr (a1)",           [49]),
]

OPTICAL_FLOW_GROUP_SHORT: Dict[int, str] = {
    0: "VLE curr",
    1: "VLE prev",
    **{i: "SAD0+init" for i in (2, 3, 4, 5)},
    **{i: "SAD right" for i in (6, 7, 8, 9, 10, 11, 12, 13)},
    **{i: "SAD left"  for i in (14, 15, 16, 17, 18, 19, 20, 21)},
    22: "CSR flip",
    **{i: "SAD down V" for i in (23, 24, 25, 26, 27)},
    28: "CSR flip",
    **{i: "SAD down H" for i in (29, 30, 31)},
    32: "CSR flip",
    **{i: "SAD up V"   for i in (33, 34, 35, 36, 37)},
    38: "CSR flip",
    **{i: "SAD up H"   for i in (39, 40, 41)},
    **{i: "Deadband"   for i in (42, 43, 44, 45, 46)},
    47: "Scale x50",
    48: "VSE motion",
    49: "VSE prev<-curr",
}


# -----------------------------------------------------------------------------
# matmul_8bitraw_short_perf.c -- CSV has 14 logical rows:
# 6 preamble words, 7 body words aggregated across k=127..0, and 1 postamble.
# -----------------------------------------------------------------------------

MATMUL_SHORT_GROUPS: List[Tuple[str, List[int]]] = [
    ("VLE-load A (a0)",          [0]),
    ("H->V CSR flip",            [1]),
    ("VLE-load B^T (a1)",        [2]),
    ("V->H CSR flip",            [3]),
    ("Init accumulators",        [4, 5]),
    ("Body shift result x128",   [6, 7]),
    ("Body H->V CSR x128",       [8]),
    ("Body gather B col x128",   [9]),
    ("Body V->H CSR x128",       [10]),
    ("Body multiply x128",       [11]),
    ("Body reduce into v7 x128", [12]),
    ("VSE-store C (a2)",         [13]),
]

MATMUL_SHORT_GROUP_SHORT: Dict[int, str] = {
    0: "VLE A",
    1: "CSR flip",
    2: "VLE B^T",
    3: "CSR flip",
    4: "Init",
    5: "Init",
    6: "Shift x128",
    7: "Shift x128",
    8: "CSR x128",
    9: "Gather x128",
    10: "CSR x128",
    11: "Mul x128",
    12: "Reduce x128",
    13: "VSE C",
}


@dataclass
class InstrSummary:
    idx: int
    mnemonic: str
    is_csr_flip: bool
    cycles_mean: float
    cycles_std: float
    wall_mean: float
    wall_std: float


def load_per_instr_csv(csv_path: str) -> List[InstrSummary]:
    """Load a per-instruction perf CSV. The schema is shared by sobel_perf
    and optical_flow_perf (and any future kernel-perf binary): the columns
    iter,instr_idx,mnemonic,is_csr_flip,t1_cycles,wall_us are universal.
    Returns one InstrSummary per distinct instr_idx with mean/std over all
    iters."""
    per_idx_cycles: Dict[int, List[float]] = defaultdict(list)
    per_idx_wall: Dict[int, List[float]] = defaultdict(list)
    mnemonic_of: Dict[int, str] = {}
    csrflip_of: Dict[int, bool] = {}

    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            i = int(row["instr_idx"])
            per_idx_cycles[i].append(float(row["t1_cycles"]))
            per_idx_wall[i].append(float(row["wall_us"]))
            mnemonic_of[i] = row["mnemonic"]
            csrflip_of[i] = int(row["is_csr_flip"]) != 0

    if not per_idx_cycles:
        sys.exit(f"no rows in {csv_path}")

    out = []
    for i in sorted(per_idx_cycles.keys()):
        cy = np.array(per_idx_cycles[i])
        ws = np.array(per_idx_wall[i])
        out.append(InstrSummary(
            idx=i, mnemonic=mnemonic_of[i], is_csr_flip=csrflip_of[i],
            cycles_mean=float(cy.mean()), cycles_std=float(cy.std()),
            wall_mean=float(ws.mean()),   wall_std=float(ws.std()),
        ))
    return out


# Backward-compat alias so existing callers keep working.
load_sobel = load_per_instr_csv


def _draw_single_bar_dual_axis(fig, ax, segments,
                               cy_total, w_total,
                               inline_label_for, legend_lines, palette,
                               legend_loc="right", legend_ncol=1,
                               label_thresh_frac=0.04,
                               header_line: str = None,
                               data_per_col: list = None,
                               bar_y: float = 0.0,
                               bar_height: float = 0.55,
                               ylim: tuple = (-0.5, 0.5)):
    """
    Draw ONE horizontal bar with TWO scales on the same rectangle:
      - top x-axis    -> T1 hardware cycles
      - bottom x-axis -> wall-clock microseconds

    Each segment's width follows its wall-us value (bottom axis is the
    primary, source-of-truth axis). The top cycle scale is rendered as a
    secondary axis using the total-cycles / total-wall conversion ratio;
    per-segment cycles vs wall ratios differ by libt1 overhead (typically
    <1%) which is below visual resolution.
    """
    label_thresh = w_total * label_thresh_frac

    left = 0.0
    for seg, color in zip(segments, palette):
        ax.barh([bar_y], [seg["wall"]], left=left, height=bar_height,
                color=color, edgecolor="black", linewidth=0.4)
        if seg["wall"] >= label_thresh:
            ax.text(left + seg["wall"] / 2, bar_y,
                    inline_label_for(seg), ha="center", va="center",
                    fontsize=8, color="black")
        left += seg["wall"]

    ax.set_yticks([])
    ax.set_ylim(ylim)
    ax.set_xlim(0, w_total * 1.02)
    ax.set_xlabel("wall-clock microseconds (mean)")

    # Top axis: cycles, derived linearly from wall via the total-cy / total-wall
    # ratio. For sobel that's ~60 cyc/us = 60 MHz T1 clock; the small libt1
    # offset per op is invisible at the bar's resolution.
    if w_total > 0:
        ratio = cy_total / w_total
        secax = ax.secondary_xaxis(
            "top",
            functions=(lambda us, r=ratio: us * r,
                       lambda c, r=ratio: c / r if r > 0 else 0.0))
        secax.set_xlabel("T1 hardware cycles (mean)")

    data_handles = [Patch(facecolor=c, edgecolor="black", label=l)
                    for l, c in zip(legend_lines, palette)]

    # If a header was provided, repeat it once at the top of each column
    # so every column shows the column labels.
    #
    # matplotlib's Legend lays out entries column-major. It always puts
    # the "extra" entries in the FIRST columns (per its divmod algorithm
    # in Legend._init_legend_box). When the caller specifies a custom
    # data-per-column distribution that doesn't match that natural
    # behaviour, we pad each column with invisible entries so every
    # column has the same row count -- which forces matplotlib's default
    # to land on the desired distribution.
    if header_line is not None and legend_ncol > 0:
        header_patch = Patch(facecolor="none", edgecolor="none",
                             label=header_line)
        pad_patch = Patch(facecolor="none", edgecolor="none", label=" ")
        ndata = len(data_handles)

        if data_per_col is None:
            # Mirror matplotlib's natural divmod distribution so our
            # headers align with where matplotlib would put column
            # boundaries.
            total = ndata + legend_ncol
            nrows, num_large = divmod(total, legend_ncol)
            per_col = [nrows if i >= num_large else nrows + 1
                       for i in range(legend_ncol)]
            data_per_col = [n - 1 for n in per_col]

        assert sum(data_per_col) == ndata, (
            f"data_per_col sum {sum(data_per_col)} != ndata {ndata}")
        assert len(data_per_col) == legend_ncol

        # Each column carries 1 header + its data entries. To force
        # matplotlib to use exactly N rows per column we pad shorter
        # columns to the longest column's height with invisible patches.
        per_col_total = [n + 1 for n in data_per_col]
        nrows_target = max(per_col_total)

        legend_handles: List[Patch] = []
        header_positions: List[int] = []
        cursor = 0
        for n_data in data_per_col:
            header_positions.append(len(legend_handles))
            legend_handles.append(header_patch)
            legend_handles.extend(data_handles[cursor:cursor + n_data])
            cursor += n_data
            # Pad this column up to nrows_target so each column ends up
            # the same height -- matplotlib will then split entries
            # equally across columns, putting our headers at the right
            # row-0 slots.
            n_in_col = 1 + n_data
            while n_in_col < nrows_target:
                legend_handles.append(pad_patch)
                n_in_col += 1
    else:
        legend_handles = data_handles
        header_positions = []

    # Per-row spacing (within a column) is kept tight (`labelspacing`
    # small); per-column spacing (between adjacent columns) is enlarged
    # so the visual blocks are clearly separated. Handle/text gap is
    # also kept small so rows hug their colour marker.
    LEG_KW = dict(fontsize=8, frameon=False, prop={"family": "monospace"},
                  labelspacing=0.25, columnspacing=4.5,
                  handletextpad=0.4, handlelength=1.5,
                  borderaxespad=0.3)

    if legend_loc == "right":
        # Sit right next to the bar's right edge; bbox is in figure coords.
        leg = fig.legend(handles=legend_handles, loc="center left",
                         bbox_to_anchor=(0.74, 0.5), ncol=legend_ncol,
                         **LEG_KW)
    else:  # bottom
        # Anchor just below the bar's xlabel (axes-fraction y < 0) so the
        # legend sits right under the bottom axis instead of floating at
        # the figure base.
        leg = fig.legend(handles=legend_handles, loc="upper center",
                         bbox_to_anchor=(0.5, -0.55),
                         bbox_transform=ax.transAxes,
                         ncol=legend_ncol, **LEG_KW)

    # Bold every header row so each column's header stands out.
    leg_texts = leg.get_texts()
    for pos in header_positions:
        if pos < len(leg_texts):
            leg_texts[pos].set_fontweight("bold")


def plot_sobel_per_instr(instrs: List[InstrSummary], out_path: str) -> None:
    cy_total = sum(i.cycles_mean for i in instrs)
    w_total = sum(i.wall_mean for i in instrs)
    palette = plt.cm.tab20(np.linspace(0, 1, len(instrs)))

    segments = [{"idx": i.idx, "mnem": i.mnemonic,
                 "cyc": i.cycles_mean, "wall": i.wall_mean,
                 "is_csr": i.is_csr_flip,
                 "group": SOBEL_GROUP_SHORT.get(i.idx, "?")}
                for i in instrs]

    # Legend layout: stage | assembly | group | mean cycles | mean us.
    # All monospace and right- / left- padded so columns line up
    # vertically across rows AND with the bold header below.
    #
    # Column widths are sized to the longest content that needs to fit:
    #   stage  -- header "stage" (5) > largest data "18:" (3)
    #   group  -- "|Gx|+|Gy|" is the widest at 9 chars
    #   cycles -- header "cycles" (6) >= largest data "13.6k" (5)
    #   wall   -- "295us" (5) padded to 5 with one-space margin via the
    #             "  " literal separator after `cyc`
    IDX_W = 5
    MNEM_W = 14
    GRP_W = 9
    CYC_W = 6
    WALL_W = 5

    def _row(idx: str, mnem: str, group: str, cyc: str, wall: str) -> str:
        return (f"{idx:>{IDX_W}s} "
                f"{mnem:<{MNEM_W}s} "
                f"{group:<{GRP_W}s} "
                f"{cyc:>{CYC_W}s}  "
                f"{wall:>{WALL_W}s}")

    legend_lines = [
        _row(f"{s['idx']}:", s["mnem"], s["group"],
             _human_cyc(s["cyc"]), _human_us(s["wall"]))
        for s in segments
    ]
    # Prepend the bold-rendered header. An invisible patch holds its
    # place so the header gets the same colour-marker indent as the
    # data rows.
    header_line = _row("stage", "assembly", "group", "cycles", "wall")

    # Single axes hosting BOTH the per-stage "assembly" bar AND a thin
    # "group" strip directly above it (group on top, assembly below).
    # Group cells use alternating neutral grays so they demarcate phase
    # boundaries without drawing attention away from the assembly bar.
    fig = plt.figure(figsize=(16, 4.8))
    gs = fig.add_gridspec(1, 1,
                          left=0.07, right=0.98, top=0.74, bottom=0.46)
    ax = fig.add_subplot(gs[0])

    def inline(seg):
        return f"{seg['idx']}"

    # Geometry: group strip ON TOP, assembly bar BELOW, tight gap.
    # Both share the same x-axis (wall-us / cycles).
    GROUP_Y    = 0.35
    GROUP_H    = 0.25
    ASSEMBLY_Y = 0.00
    ASSEMBLY_H = 0.30
    SEP_Y      = 0.19   # dotted horizontal line between the two rows
    Y_TOP      = 0.50
    Y_BOTTOM   = -0.20

    _draw_single_bar_dual_axis(
        fig, ax, segments,
        cy_total=cy_total, w_total=w_total,
        inline_label_for=inline,
        legend_lines=legend_lines,
        palette=palette,
        # 3 columns split by logical kernel phase rather than
        # matplotlib's natural divmod:
        #   col 0 -- VLE-load + Gx pass        (stages 0-5,  6 data)
        #   col 1 -- H->V CSR flip + Gy pass   (stages 6-11, 6 data)
        #   col 2 -- V->H CSR flip + abs/sat + VSE-store (stages 12-18, 7 data)
        legend_loc="bottom", legend_ncol=3,
        data_per_col=[6, 6, 7],
        label_thresh_frac=0.018,
        header_line=header_line,
        bar_y=ASSEMBLY_Y, bar_height=ASSEMBLY_H,
        ylim=(Y_BOTTOM, Y_TOP),
    )

    # ------------------------------------------------------------------
    # Group strip: ABOVE the assembly bar. Alternating neutral grays
    # (checkerboard, same tone) just demarcate phase boundaries -- the
    # vivid colours stay on the assembly bar where the data lives.
    # ------------------------------------------------------------------
    GRAY_LIGHT = "#F2F2F2"
    GRAY_DARK  = "#A8A8A8"
    cumsum = np.cumsum([0.0] + [s["wall"] for s in segments])
    label_thresh_strip = w_total * 0.03
    drawn = 0
    boundaries = set()
    for label, idxs in SOBEL_GROUPS:
        if not idxs:
            continue
        left = cumsum[idxs[0]]
        right = cumsum[idxs[-1] + 1]
        width = right - left
        boundaries.add(right)
        if width <= 0:
            continue
        color = GRAY_LIGHT if drawn % 2 == 0 else GRAY_DARK
        ax.barh([GROUP_Y], [width], left=left, height=GROUP_H,
                color=color, edgecolor="black", linewidth=0.4)
        short = label.split(" ")[0]
        if width >= label_thresh_strip:
            ax.text(left + width / 2, GROUP_Y, short,
                    ha="center", va="center", fontsize=8)
        drawn += 1

    # Drop the rightmost boundary (= w_total, the bar's right edge).
    boundaries.discard(max(boundaries))

    # Vertical dotted lines at every internal group boundary. We use
    # data coordinates plus clip_on=False so the lines cross the axes
    # box border and stick out a touch above and below, making the
    # phase split visually obvious.
    line_top = Y_TOP + 0.08
    line_bot = Y_BOTTOM - 0.08
    for x in sorted(boundaries):
        ax.plot([x, x], [line_bot, line_top],
                linestyle=":", color="dimgray", linewidth=1.1,
                clip_on=False, zorder=5)

    # Re-apply y-ticks AFTER the helper cleared them: label each row
    # on the left so the chart reads "group" / "assembly" without a
    # legend lookup.
    ax.set_yticks([ASSEMBLY_Y, GROUP_Y])
    ax.set_yticklabels(["assembly", "group"], fontsize=9)
    ax.tick_params(axis="y", length=0)
    fig.suptitle(
        f"Sobel kernel: per-instruction breakdown   "
        f"19 issued ops + 2 dispatcher-only CSR flips\n"
        f"total T1 cycles = {_human_cyc(cy_total)}    "
        f"total wall = {_human_us(w_total)}    "
        f"libt1 sliver = {_human_us(max(0, w_total - cy_total/60))} (@60MHz)",
        fontsize=11, y=0.97,
    )
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_sobel_grouped(instrs: List[InstrSummary], out_path: str) -> None:
    palette = plt.cm.Set2(np.linspace(0, 1, len(SOBEL_GROUPS)))
    segments = []
    for label, idxs in SOBEL_GROUPS:
        cy = sum(i.cycles_mean for i in instrs if i.idx in idxs)
        w  = sum(i.wall_mean   for i in instrs if i.idx in idxs)
        segments.append({"idx": idxs[0] if len(idxs) == 1 else f"{idxs[0]}-{idxs[-1]}",
                         "mnem": label, "cyc": cy, "wall": w,
                         "is_csr": False})

    cy_total = sum(s["cyc"] for s in segments)
    w_total = sum(s["wall"] for s in segments)
    legend_lines = [
        f"{s['mnem']:<26s} {_human_cyc(s['cyc']):>7s}  {_human_us(s['wall']):>7s}"
        for s in segments
    ]
    header_line = f"{'group':<26s} {'cycles':>7s}  {'wall':>7s}"

    fig = plt.figure(figsize=(16, 3.0))
    gs = fig.add_gridspec(1, 1, left=0.05, right=0.72, top=0.66, bottom=0.34)
    ax = fig.add_subplot(gs[0])

    def inline(seg):
        # Wide segments can fit the group name (first word only).
        return f"{seg['mnem'].split(' ')[0]}"

    _draw_single_bar_dual_axis(
        fig, ax, segments,
        cy_total=cy_total, w_total=w_total,
        inline_label_for=inline,
        legend_lines=legend_lines,
        palette=palette,
        legend_loc="right", legend_ncol=1,
        header_line=header_line,
    )
    fig.suptitle(
        f"Sobel kernel: breakdown by logical group   "
        f"total T1 = {_human_cyc(cy_total)}    "
        f"total wall = {_human_us(w_total)}",
        fontsize=11, y=0.96,
    )
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_sobel(csv_path: str, out_path: str, mode: str) -> None:
    instrs = load_per_instr_csv(csv_path)
    base, ext = os.path.splitext(out_path)
    ext = ext or ".png"
    if mode in ("per-instr", "both"):
        path = out_path if mode == "per-instr" else f"{base}.per_instr{ext}"
        plot_sobel_per_instr(instrs, path)
    if mode in ("grouped", "both"):
        path = out_path if mode == "grouped" else f"{base}.grouped{ext}"
        plot_sobel_grouped(instrs, path)


# -----------------------------------------------------------------------------
# Optical-flow plotting -- mirrors the sobel functions above, parameterised
# on OPTICAL_FLOW_GROUPS / OPTICAL_FLOW_GROUP_SHORT. The wider 50-word
# kernel needs a 3-column legend split with a heavier middle/end column
# because the SAD_left/down/up and deadband phases land there.
# -----------------------------------------------------------------------------

def plot_optical_flow_per_instr(instrs: List[InstrSummary],
                                out_path: str) -> None:
    cy_total = sum(i.cycles_mean for i in instrs)
    w_total  = sum(i.wall_mean   for i in instrs)
    palette = plt.cm.tab20(np.linspace(0, 1, len(instrs)))

    segments = [{"idx": i.idx, "mnem": i.mnemonic,
                 "cyc": i.cycles_mean, "wall": i.wall_mean,
                 "is_csr": i.is_csr_flip,
                 "group": OPTICAL_FLOW_GROUP_SHORT.get(i.idx, "?")}
                for i in instrs]

    IDX_W = 5
    MNEM_W = 14
    GRP_W = 13   # OPTICAL_FLOW_GROUP_SHORT has longer labels than sobel
    CYC_W = 6
    WALL_W = 5

    def _row(idx: str, mnem: str, group: str, cyc: str, wall: str) -> str:
        return (f"{idx:>{IDX_W}s} "
                f"{mnem:<{MNEM_W}s} "
                f"{group:<{GRP_W}s} "
                f"{cyc:>{CYC_W}s}  "
                f"{wall:>{WALL_W}s}")

    legend_lines = [
        _row(f"{s['idx']}:", s["mnem"], s["group"],
             _human_cyc(s["cyc"]), _human_us(s["wall"]))
        for s in segments
    ]
    header_line = _row("stage", "assembly", "group", "cycles", "wall")

    fig = plt.figure(figsize=(16, 5.6))
    gs = fig.add_gridspec(1, 1,
                          left=0.07, right=0.98, top=0.74, bottom=0.40)
    ax = fig.add_subplot(gs[0])

    def inline(seg):
        return f"{seg['idx']}"

    GROUP_Y    = 0.35
    GROUP_H    = 0.25
    ASSEMBLY_Y = 0.00
    ASSEMBLY_H = 0.30
    Y_TOP      = 0.50
    Y_BOTTOM   = -0.20

    n = len(instrs)
    # Phase-based 3-column split for the 50-word kernel. Adjusts if the
    # kernel shrinks for some reason (e.g. early-return harness test).
    if n == 50:
        data_per_col = [14, 18, 18]   # 0-13, 14-31, 32-49
    else:
        base = n // 3
        rem = n - base * 3
        data_per_col = [base + (1 if k < rem else 0) for k in range(3)]

    _draw_single_bar_dual_axis(
        fig, ax, segments,
        cy_total=cy_total, w_total=w_total,
        inline_label_for=inline,
        legend_lines=legend_lines,
        palette=palette,
        legend_loc="bottom", legend_ncol=3,
        data_per_col=data_per_col,
        label_thresh_frac=0.012,
        header_line=header_line,
        bar_y=ASSEMBLY_Y, bar_height=ASSEMBLY_H,
        ylim=(Y_BOTTOM, Y_TOP),
    )

    # Group strip (same approach as plot_sobel_per_instr).
    GRAY_LIGHT = "#F2F2F2"
    GRAY_DARK  = "#A8A8A8"
    cumsum = np.cumsum([0.0] + [s["wall"] for s in segments])
    label_thresh_strip = w_total * 0.03
    drawn = 0
    boundaries = set()
    for label, idxs in OPTICAL_FLOW_GROUPS:
        if not idxs:
            continue
        left = cumsum[idxs[0]]
        right = cumsum[idxs[-1] + 1]
        width = right - left
        boundaries.add(right)
        if width <= 0:
            continue
        color = GRAY_LIGHT if drawn % 2 == 0 else GRAY_DARK
        ax.barh([GROUP_Y], [width], left=left, height=GROUP_H,
                color=color, edgecolor="black", linewidth=0.4)
        # Label the strip with the same short group name shown in the legend
        # "group" column (keyed off the group's first instr index), instead
        # of label.split()[0] -- the latter rendered the down/up V-mode
        # compute blocks as a bare "V-mode" and the deadband as "Static"
        # (which collided with the opening "SAD_static").
        short = OPTICAL_FLOW_GROUP_SHORT.get(idxs[0], label.split(" ")[0])
        if width >= label_thresh_strip:
            ax.text(left + width / 2, GROUP_Y, short,
                    ha="center", va="center", fontsize=8)
        drawn += 1

    boundaries.discard(max(boundaries))
    line_top = Y_TOP + 0.08
    line_bot = Y_BOTTOM - 0.08
    for x in sorted(boundaries):
        ax.plot([x, x], [line_bot, line_top],
                linestyle=":", color="dimgray", linewidth=1.1,
                clip_on=False, zorder=5)

    ax.set_yticks([ASSEMBLY_Y, GROUP_Y])
    ax.set_yticklabels(["assembly", "group"], fontsize=9)
    ax.tick_params(axis="y", length=0)

    n_issued = sum(1 for s in segments if not s["is_csr"])
    n_csr    = sum(1 for s in segments if s["is_csr"])
    fig.suptitle(
        f"Optical-flow kernel (5-direction block matching): "
        f"per-instruction breakdown   "
        f"{n_issued} issued ops + {n_csr} dispatcher-only CSR flips\n"
        f"total T1 cycles = {_human_cyc(cy_total)}    "
        f"total wall = {_human_us(w_total)}    "
        f"libt1 sliver = {_human_us(max(0, w_total - cy_total/60))} (@60MHz)",
        fontsize=11, y=0.97,
    )
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_optical_flow_grouped(instrs: List[InstrSummary],
                              out_path: str) -> None:
    palette = plt.cm.Set2(np.linspace(0, 1, len(OPTICAL_FLOW_GROUPS)))
    segments = []
    for label, idxs in OPTICAL_FLOW_GROUPS:
        cy = sum(i.cycles_mean for i in instrs if i.idx in idxs)
        w  = sum(i.wall_mean   for i in instrs if i.idx in idxs)
        segments.append({"idx": idxs[0] if len(idxs) == 1
                                 else f"{idxs[0]}-{idxs[-1]}",
                         "mnem": label, "cyc": cy, "wall": w,
                         "is_csr": False})

    cy_total = sum(s["cyc"] for s in segments)
    w_total  = sum(s["wall"] for s in segments)
    legend_lines = [
        f"{s['mnem']:<30s} {_human_cyc(s['cyc']):>7s}  {_human_us(s['wall']):>7s}"
        for s in segments
    ]
    header_line = f"{'group':<30s} {'cycles':>7s}  {'wall':>7s}"

    fig = plt.figure(figsize=(16, 4.0))
    gs = fig.add_gridspec(1, 1, left=0.05, right=0.68, top=0.66, bottom=0.34)
    ax = fig.add_subplot(gs[0])

    def inline(seg):
        return f"{seg['mnem'].split(' ')[0]}"

    _draw_single_bar_dual_axis(
        fig, ax, segments,
        cy_total=cy_total, w_total=w_total,
        inline_label_for=inline,
        legend_lines=legend_lines,
        palette=palette,
        legend_loc="right", legend_ncol=1,
        header_line=header_line,
    )
    fig.suptitle(
        f"Optical-flow kernel: breakdown by logical group   "
        f"total T1 = {_human_cyc(cy_total)}    "
        f"total wall = {_human_us(w_total)}",
        fontsize=11, y=0.96,
    )
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_optical_flow(csv_path: str, out_path: str, mode: str) -> None:
    instrs = load_per_instr_csv(csv_path)
    base, ext = os.path.splitext(out_path)
    ext = ext or ".png"
    if mode in ("per-instr", "both"):
        path = out_path if mode == "per-instr" else f"{base}.per_instr{ext}"
        plot_optical_flow_per_instr(instrs, path)
    if mode in ("grouped", "both"):
        path = out_path if mode == "grouped" else f"{base}.grouped{ext}"
        plot_optical_flow_grouped(instrs, path)


# -----------------------------------------------------------------------------
# matmul_8bitraw_short plotting -- same per-instruction CSV schema as sobel and
# optical-flow, but body rows are accumulated over all 128 output columns.
# -----------------------------------------------------------------------------

def plot_matmul_8bitraw_short_per_instr(instrs: List[InstrSummary],
                                        out_path: str) -> None:
    cy_total = sum(i.cycles_mean for i in instrs)
    w_total  = sum(i.wall_mean   for i in instrs)
    palette = plt.cm.tab20(np.linspace(0, 1, len(instrs)))

    def display_mnemonic(mnem: str) -> str:
        # Keep the legend's monospace columns aligned even when plotting
        # CSVs produced before the CSR marker labels were shortened.
        return (mnem
                .replace("csrwi(vmode=1) x128", "csrwi V=1 x128")
                .replace("csrwi(vmode=0) x128", "csrwi V=0 x128"))

    segments = [{"idx": i.idx, "mnem": i.mnemonic,
                 "cyc": i.cycles_mean, "wall": i.wall_mean,
                 "is_csr": i.is_csr_flip,
                 "group": MATMUL_SHORT_GROUP_SHORT.get(i.idx, "?")}
                for i in instrs]

    rendered_mnems = [display_mnemonic(s["mnem"]) for s in segments]
    rendered_cycles = [_human_cyc(s["cyc"]) for s in segments]
    rendered_walls = [_human_us(s["wall"]) for s in segments]

    IDX_W = max(len("stage"), max(len(f"{s['idx']}:") for s in segments))
    MNEM_W = max(len("assembly"), max(len(m) for m in rendered_mnems))
    GRP_W = max(len("group"), max(len(s["group"]) for s in segments))
    CYC_W = max(len("cycles"), max(len(c) for c in rendered_cycles))
    WALL_W = max(len("wall"), max(len(w) for w in rendered_walls))

    def _row(idx: str, mnem: str, group: str, cyc: str, wall: str) -> str:
        return (f"{idx:>{IDX_W}s} "
                f"{mnem:<{MNEM_W}s} "
                f"{group:<{GRP_W}s} "
                f"{cyc:>{CYC_W}s}  "
                f"{wall:>{WALL_W}s}")

    legend_lines = [
        _row(f"{s['idx']}:", mnem, s["group"], cyc, wall)
        for s, mnem, cyc, wall in zip(segments, rendered_mnems,
                                      rendered_cycles, rendered_walls)
    ]
    header_line = _row("stage", "assembly", "group", "cycles", "wall")

    fig = plt.figure(figsize=(16, 4.8))
    gs = fig.add_gridspec(1, 1,
                          left=0.07, right=0.98, top=0.74, bottom=0.40)
    ax = fig.add_subplot(gs[0])

    def inline(seg):
        return f"{seg['idx']}"

    GROUP_Y    = 0.35
    GROUP_H    = 0.25
    ASSEMBLY_Y = 0.00
    ASSEMBLY_H = 0.30
    Y_TOP      = 0.50
    Y_BOTTOM   = -0.20

    n = len(instrs)
    data_per_col = [n // 2, n - (n // 2)]

    _draw_single_bar_dual_axis(
        fig, ax, segments,
        cy_total=cy_total, w_total=w_total,
        inline_label_for=inline,
        legend_lines=legend_lines,
        palette=palette,
        legend_loc="bottom", legend_ncol=2,
        data_per_col=data_per_col,
        label_thresh_frac=0.018,
        header_line=header_line,
        bar_y=ASSEMBLY_Y, bar_height=ASSEMBLY_H,
        ylim=(Y_BOTTOM, Y_TOP),
    )

    GRAY_LIGHT = "#F2F2F2"
    GRAY_DARK  = "#A8A8A8"
    cumsum = np.cumsum([0.0] + [s["wall"] for s in segments])
    label_thresh_strip = w_total * 0.035
    drawn = 0
    boundaries = set()
    for label, idxs in MATMUL_SHORT_GROUPS:
        if not idxs or idxs[-1] >= len(cumsum) - 1:
            continue
        left = cumsum[idxs[0]]
        right = cumsum[idxs[-1] + 1]
        width = right - left
        boundaries.add(right)
        if width <= 0:
            continue
        color = GRAY_LIGHT if drawn % 2 == 0 else GRAY_DARK
        ax.barh([GROUP_Y], [width], left=left, height=GROUP_H,
                color=color, edgecolor="black", linewidth=0.4)
        short = MATMUL_SHORT_GROUP_SHORT.get(idxs[0], label.split(" ")[0])
        if width >= label_thresh_strip:
            ax.text(left + width / 2, GROUP_Y, short,
                    ha="center", va="center", fontsize=8)
        drawn += 1

    if boundaries:
        boundaries.discard(max(boundaries))
    line_top = Y_TOP + 0.08
    line_bot = Y_BOTTOM - 0.08
    for x in sorted(boundaries):
        ax.plot([x, x], [line_bot, line_top],
                linestyle=":", color="dimgray", linewidth=1.1,
                clip_on=False, zorder=5)

    ax.set_yticks([ASSEMBLY_Y, GROUP_Y])
    ax.set_yticklabels(["assembly", "group"], fontsize=9)
    ax.tick_params(axis="y", length=0)

    n_issued = sum(1 for s in segments if not s["is_csr"])
    n_csr    = sum(1 for s in segments if s["is_csr"])
    fig.suptitle(
        f"matmul_8bitraw_short: logical breakdown   "
        f"{n_issued} timed rows + {n_csr} dispatcher-only CSR flips; "
        f"body rows aggregate 128 repeats\n"
        f"total T1 cycles = {_human_cyc(cy_total)}    "
        f"total wall = {_human_us(w_total)}    "
        f"libt1 sliver = {_human_us(max(0, w_total - cy_total/60))} (@60MHz)",
        fontsize=11, y=0.97,
    )
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_matmul_8bitraw_short_grouped(instrs: List[InstrSummary],
                                      out_path: str) -> None:
    palette = plt.cm.Set2(np.linspace(0, 1, len(MATMUL_SHORT_GROUPS)))
    segments = []
    for label, idxs in MATMUL_SHORT_GROUPS:
        cy = sum(i.cycles_mean for i in instrs if i.idx in idxs)
        w  = sum(i.wall_mean   for i in instrs if i.idx in idxs)
        segments.append({"idx": idxs[0] if len(idxs) == 1
                                 else f"{idxs[0]}-{idxs[-1]}",
                         "mnem": label, "cyc": cy, "wall": w,
                         "is_csr": False})

    cy_total = sum(s["cyc"] for s in segments)
    w_total  = sum(s["wall"] for s in segments)
    group_w = max(len("group"), max(len(s["mnem"]) for s in segments))
    cycle_w = max(len("cycles"), max(len(_human_cyc(s["cyc"])) for s in segments))
    wall_w = max(len("wall"), max(len(_human_us(s["wall"])) for s in segments))
    legend_lines = [
        f"{s['mnem']:<{group_w}s} {_human_cyc(s['cyc']):>{cycle_w}s}  {_human_us(s['wall']):>{wall_w}s}"
        for s in segments
    ]
    header_line = f"{'group':<{group_w}s} {'cycles':>{cycle_w}s}  {'wall':>{wall_w}s}"

    fig = plt.figure(figsize=(16, 3.4))
    gs = fig.add_gridspec(1, 1, left=0.05, right=0.68, top=0.66, bottom=0.34)
    ax = fig.add_subplot(gs[0])

    def inline(seg):
        return f"{seg['mnem'].split(' ')[0]}"

    _draw_single_bar_dual_axis(
        fig, ax, segments,
        cy_total=cy_total, w_total=w_total,
        inline_label_for=inline,
        legend_lines=legend_lines,
        palette=palette,
        legend_loc="right", legend_ncol=1,
        header_line=header_line,
    )
    fig.suptitle(
        f"matmul_8bitraw_short: breakdown by logical group   "
        f"total T1 = {_human_cyc(cy_total)}    "
        f"total wall = {_human_us(w_total)}",
        fontsize=11, y=0.96,
    )
    fig.savefig(out_path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_matmul_8bitraw_short(csv_path: str, out_path: str,
                              mode: str) -> None:
    instrs = load_per_instr_csv(csv_path)
    base, ext = os.path.splitext(out_path)
    ext = ext or ".png"
    if mode in ("per-instr", "both"):
        path = out_path if mode == "per-instr" else f"{base}.per_instr{ext}"
        plot_matmul_8bitraw_short_per_instr(instrs, path)
    if mode in ("grouped", "both"):
        path = out_path if mode == "grouped" else f"{base}.grouped{ext}"
        plot_matmul_8bitraw_short_grouped(instrs, path)


# -----------------------------------------------------------------------------
# Driver
# -----------------------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    pp = sub.add_parser("pipeline", help="plot pipeline_perf CSV")
    pp.add_argument("csv")
    pp.add_argument("--out", default="pipeline_breakdown.png")
    pp.add_argument("--t1-hz", type=float, default=60e6,
                    help="T1 clock in Hz (default 60e6 for 5t-maskopt)")
    pp.add_argument("--include-warmup", action="store_true",
                    help="include frame 0 (camera/display ramp-up; default skip)")

    sp = sub.add_parser("sobel", help="plot sobel_perf CSV")
    sp.add_argument("csv")
    sp.add_argument("--out", default="sobel_breakdown.png")
    sp.add_argument("--per-instr", action="store_const", const="per-instr",
                    dest="mode", help="emit only the per-instruction chart")
    sp.add_argument("--grouped", action="store_const", const="grouped",
                    dest="mode", help="emit only the logical-group chart")
    sp.add_argument("--both", action="store_const", const="both",
                    dest="mode", help="emit both (default)")
    sp.set_defaults(mode="both")

    op = sub.add_parser("optical_flow",
                        help="plot optical_flow_perf CSV "
                             "(5-direction block-matching kernel)")
    op.add_argument("csv")
    op.add_argument("--out", default="optical_flow_breakdown.png")
    op.add_argument("--per-instr", action="store_const", const="per-instr",
                    dest="mode", help="emit only the per-instruction chart")
    op.add_argument("--grouped", action="store_const", const="grouped",
                    dest="mode", help="emit only the logical-group chart")
    op.add_argument("--both", action="store_const", const="both",
                    dest="mode", help="emit both (default)")
    op.set_defaults(mode="both")

    mp = sub.add_parser("matmul_8bitraw_short",
                        help="plot matmul_8bitraw_short_perf CSV")
    mp.add_argument("csv")
    mp.add_argument("--out", default="matmul_8bitraw_short_breakdown.png")
    mp.add_argument("--per-instr", action="store_const", const="per-instr",
                    dest="mode", help="emit only the logical-row chart")
    mp.add_argument("--grouped", action="store_const", const="grouped",
                    dest="mode", help="emit only the logical-group chart")
    mp.add_argument("--both", action="store_const", const="both",
                    dest="mode", help="emit both (default)")
    mp.set_defaults(mode="both")

    args = p.parse_args()
    if args.cmd == "pipeline":
        plot_pipeline(args.csv, args.out, args.t1_hz,
                      skip_warmup=not args.include_warmup)
    elif args.cmd == "sobel":
        plot_sobel(args.csv, args.out, args.mode)
    elif args.cmd == "optical_flow":
        plot_optical_flow(args.csv, args.out, args.mode)
    elif args.cmd == "matmul_8bitraw_short":
        plot_matmul_8bitraw_short(args.csv, args.out, args.mode)
    return 0


if __name__ == "__main__":
    sys.exit(main())
