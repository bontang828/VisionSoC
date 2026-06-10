#!/usr/bin/env python3
"""
plot_viz.py -- assemble the attention_self stage PPMs (from
attention_self_viz) into a labelled flow diagram showing what the
"visualised" 2D vector processor holds in its 128x128 data plane at each
stage of the on-fabric ViT self-attention pipeline.

Usage:
    plot_viz.py <ppm_dir> [--out OUT.png] [--style landscape|flow]

Two styles:
  landscape (default) -- stages laid LEFT-TO-RIGHT; each stage is a
      coloured panel containing its two tiles STACKED (first 128 on top,
      second 128 below); the stage name sits in the coloured strip at the
      bottom of each panel; all the explanatory text is grouped at the
      bottom of the figure, in plain language.
  flow -- the original portrait layout: one stage per row, image(s) on the
      left, explanation on the right.

Spatial images (input/patch/transpose/output) are drawn grayscale;
attention quantities (scores/weights/totals/attention map) are drawn as a
per-tile min-max-normalised heatmap so structure is visible even when the
raw magnitudes are tiny.
"""
from __future__ import annotations

import argparse
import os
import sys
import textwrap

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
import numpy as np
from PIL import Image


def load(path: str):
    if not os.path.exists(path):
        return None
    return np.asarray(Image.open(path))


# =============================================================================
# Shared stage data
# =============================================================================

# (file, mode); mode "gray" = spatial pixels, "heat" = normalised heatmap.
F_INPUT  = ("00_input_frame.ppm", "gray")
F_QA     = ("01_patch_Qa.ppm", "gray")
F_QB     = ("01_patch_Qb.ppm", "gray")
F_KTA    = ("02_transpose_Kta.ppm", "gray")
F_KTB    = ("02_transpose_Ktb.ppm", "gray")
F_SA     = ("03_scores_Sa.ppm", "heat")
F_SB     = ("03_scores_Sb.ppm", "heat")
F_EA     = ("04_exp_ea.ppm", "heat")
F_EB     = ("04_exp_eb.ppm", "heat")
F_Z      = ("05_softmax_sum_Z.ppm", "heat")
F_R      = ("05_reciprocal_R.ppm", "heat")
F_PQA    = ("06_prob_pqa.ppm", "heat")
F_PQB    = ("06_prob_pqb.ppm", "heat")
F_OA     = ("07_output_Oa.ppm", "gray")
F_OB     = ("07_output_Ob.ppm", "gray")
F_OUTPUT = ("08_output_frame.ppm", "gray")


def _show(ax, img, mode):
    if img is None:
        ax.text(0.5, 0.5, "(missing)", ha="center", va="center",
                fontsize=8, color="red")
        ax.set_xticks([]); ax.set_yticks([]); ax.set_facecolor("none")
        return
    if mode == "gray":
        ax.imshow(img, cmap="gray", vmin=0, vmax=255,
                  interpolation="nearest", aspect=ax.get_aspect())
    else:
        lo, hi = float(img.min()), float(img.max())
        ax.imshow(img, cmap="magma", vmin=lo, vmax=max(hi, lo + 1),
                  interpolation="nearest", aspect=ax.get_aspect())
    ax.set_xticks([]); ax.set_yticks([])
    for s in ax.spines.values():
        s.set_edgecolor("#666666"); s.set_linewidth(0.8)


# =============================================================================
# Style 1: landscape, paired-stacked, colour-coded stages, text at the bottom
# =============================================================================

# Plain-language stage names (NO pqa/pqb/S_SHIFT jargon). Names use 2 lines so
# they can be large without running out of the panel.
# name, type ("single"/"pair"), panels, per-tile sub-labels (what each tile is).
STAGES_LS = [
    ("Camera\nframe",    "single", [F_INPUT],       [None]),
    ("Patchify",         "pair",   [F_QA, F_QB],     ["tokens 0-127", "tokens 128-255"]),
    ("Transpose",        "pair",   [F_KTA, F_KTB],   ["keys 0-127", "keys 128-255"]),
    ("Similarity",       "pair",   [F_SA, F_SB],     ["keys 0-127", "keys 128-255"]),
    ("Softmax\nweights", "pair",   [F_EA, F_EB],     ["keys 0-127", "keys 128-255"]),
    ("Row\ntotals",      "pair",   [F_Z, F_R],       ["row sum", "1 / row sum"]),
    ("Attention\nmap",   "pair",   [F_PQA, F_PQB],   ["keys 0-127", "keys 128-255"]),
    ("Value\nmix",       "pair",   [F_OA, F_OB],     ["tokens 0-127", "tokens 128-255"]),
    ("Output\nframe",    "single", [F_OUTPUT],       [None]),
]

# Soft, distinct panel colours (one per stage), pleasant in print.
STAGE_COLORS = [
    "#FBE3E0", "#FCEFD4", "#E9F5D6", "#D7F0E6", "#D6E8F7",
    "#E3DCF4", "#F7D9EC", "#EDEDED", "#DCEBCF",
]

# Plain-language explanation for each stage (no ">>", no fixed-point jargon).
LS_TEXT = [
    ("1", "Camera frame",
     "The live 128x128 image the processor starts from "
     "(row = image row, column = image column)."),
    ("2", "Patchify",
     "The image is sliced into 8x8 squares; each square becomes one token "
     "(one row of data). 256 squares -> two stacked tiles."),
    ("3", "Transpose",
     "The same tokens laid out the other way round so each one can be "
     "compared against every other token."),
    ("4", "Similarity",
     "How alike each pair of squares is (brighter = more alike), rescaled "
     "to a 0-255 range. Row = a query square, column = a compared square."),
    ("5", "Softmax weights",
     "The similarities turned into positive attention weights; an "
     "exponential look-up makes strong matches stand out and weak ones fade."),
    ("6", "Row totals",
     "For each query (row): the sum of its weights (top) and one-divided-by "
     "that-sum (bottom), used to turn the weights into fractions adding to 1."),
    ("7", "Attention map",
     "The finished attention: for each query (row), the fraction of its "
     "output taken from every other square (column) - where each square looks."),
    ("8", "Value mix",
     "Each output token is a blend of all the squares, mixed by the attention "
     "map. The 64 numbers per token are its new 8x8 content."),
    ("9", "Output frame",
     "The output tokens dropped back into their 8x8 positions, rebuilding the "
     "128x128 image shown on the HDMI screen."),
]

# Headline note placed directly under the coloured panels (important).
LS_PIPELINE_NOTE = ("The whole pipeline runs on the vector fabric  -  "
                    "no CPU maths.")


def _darker(hex_color, f=0.62):
    h = hex_color.lstrip("#")
    r, g, b = (int(h[i:i+2], 16) / 255.0 for i in (0, 2, 4))
    return (r * f, g * f, b * f, 1.0)


def _place_image(fig, ppm_dir, panel, box, aspect):
    fname, mode = panel
    img = load(os.path.join(ppm_dir, fname))
    ax = fig.add_axes(box, zorder=2)
    ax.set_aspect(aspect)
    ax.set_facecolor("none")          # let the stage colour show through
    _show(ax, img, mode)
    return ax


def compose_landscape(ppm_dir: str, out: str) -> int:
    N = len(STAGES_LS)
    fig = plt.figure(figsize=(24, 11))
    bg = fig.add_axes([0, 0, 1, 1]); bg.set_xlim(0, 1); bg.set_ylim(0, 1)
    bg.axis("off")

    NAME_FS = 23      # stage-name label (big, 2 lines)
    TAG_FS  = 16      # per-tile "what is it" label

    # --- geometry (figure fraction) -- stages packed tight along the top ---
    X0, X1 = 0.010, 0.990
    GAP = 0.006
    stage_w = (X1 - X0 - GAP * (N - 1)) / N

    RECT_TOP = 0.950
    RECT_BOT = 0.430
    LABEL_H  = 0.078                      # coloured strip (holds a 2-line name)
    IMG_TOP  = RECT_TOP - 0.008
    IMG_BOT  = RECT_BOT + LABEL_H + 0.006
    IMG_MIDY = (IMG_TOP + IMG_BOT) / 2.0

    TAG_H = 0.026                         # room above each tile for its label
    VGAP  = 0.026                         # space between the two stacked tiles

    for i, (name, kind, panels, sublabels) in enumerate(STAGES_LS):
        x = X0 + i * (stage_w + GAP)
        color = STAGE_COLORS[i % len(STAGE_COLORS)]

        rect = FancyBboxPatch((x, RECT_BOT), stage_w, RECT_TOP - RECT_BOT,
                              boxstyle="round,pad=0,rounding_size=0.009",
                              linewidth=1.3, edgecolor=_darker(color),
                              facecolor=color, zorder=1, mutation_aspect=0.46)
        bg.add_patch(rect)
        bg.text(x + stage_w / 2, RECT_BOT + LABEL_H / 2, name,
                ha="center", va="center", ma="center", fontsize=NAME_FS,
                fontweight="bold", color="#1a1a1a", linespacing=1.0, zorder=3)

        pad_x = 0.005
        ax_x, ax_w = x + pad_x, stage_w - 2 * pad_x
        if kind == "single":
            _place_image(fig, ppm_dir, panels[0],
                         [ax_x, IMG_BOT, ax_w, IMG_TOP - IMG_BOT], "equal")
        else:
            img_h = (IMG_TOP - IMG_BOT - 2 * TAG_H - VGAP) / 2.0
            # top tile (a) with its range label above it
            ya = IMG_TOP - TAG_H - img_h
            bg.text(x + stage_w / 2, IMG_TOP - TAG_H / 2, sublabels[0],
                    ha="center", va="center", fontsize=TAG_FS,
                    fontweight="bold", color="#1a1a1a", zorder=3)
            _place_image(fig, ppm_dir, panels[0], [ax_x, ya, ax_w, img_h], "auto")
            # bottom tile (b) with its range label above it
            yb = ya - VGAP - TAG_H - img_h
            bg.text(x + stage_w / 2, ya - VGAP - TAG_H / 2, sublabels[1],
                    ha="center", va="center", fontsize=TAG_FS,
                    fontweight="bold", color="#1a1a1a", zorder=3)
            _place_image(fig, ppm_dir, panels[1], [ax_x, yb, ax_w, img_h], "auto")

        if i < N - 1:                    # flow chevron in the gap
            bg.text(x + stage_w + GAP / 2, IMG_MIDY, "›",
                    ha="center", va="center", fontsize=26,
                    color="#777777", zorder=3)

    bg.text(0.5, 0.990,
            "attention_self  -  the 128x128 data plane at every stage "
            "(left to right)",
            ha="center", va="top", fontsize=20, fontweight="bold",
            color="#1a3b6b", zorder=3)

    # --- important headline, directly under the coloured panels ---
    bg.text(0.5, RECT_BOT - 0.014, LS_PIPELINE_NOTE, ha="center", va="top",
            fontsize=20, fontweight="bold", color="#0a7d3c", zorder=3)

    # --- bottom: plain-language explanations in 3 columns (big, fills space) ---
    bg.text(0.010, 0.362, "How to read each stage:", ha="left", va="top",
            fontsize=16, fontweight="bold", color="#1a3b6b")
    ncol = 3
    col_x = [0.012, 0.345, 0.678]
    col_w_chars = 58
    per_col = (len(LS_TEXT) + ncol - 1) // ncol
    for k, (num, nm, txt) in enumerate(LS_TEXT):
        c = k // per_col
        r = k % per_col
        y = 0.322 - r * 0.107
        body = textwrap.fill(txt, width=col_w_chars)
        bg.text(col_x[c], y, f"{num}. {nm}", ha="left", va="top",
                fontsize=14, fontweight="bold", color="#333333")
        bg.text(col_x[c], y - 0.028, body, ha="left", va="top",
                fontsize=12.5, color="black", linespacing=1.18)

    fig.savefig(out, dpi=140)
    plt.close(fig)
    print(f"wrote {out}")
    return 0


# =============================================================================
# Style 2: original portrait flow (image left, explanation right, one per row)
# =============================================================================

STAGES_FLOW = [
    ("0 - INPUT",
     "Live 128x128 camera Y plane loaded into the VRF. This is the raw scene "
     "the 2D vector processor starts from.\nrow = image row, col = image col.",
     [F_INPUT]),
    ("1 - im2col  (patchify, on fabric)",
     "Each 8x8 pixel patch is gathered into ONE token = one VRF row. "
     "256 tokens -> two 128-row tiles Qa / Qb.\nrow = token, "
     "col 0..63 = the patch's 64 pixels (cols 64..127 zero-pad).",
     [F_QA, F_QB]),
    ("2 - transpose  (K = patches^T, on fabric)",
     "The patches re-laid so the QK matmul can stream keys down the lanes.\n"
     "row = feature, col = key.",
     [F_KTA, F_KTB]),
    ("3 - QK scores",
     "Dot-product similarity of every query patch against every key patch "
     "(rescaled to 0-255). Bright = similar.\nrow = query, col = key.",
     [F_SA, F_SB]),
    ("4 - softmax weights + row totals",
     "exp(best - score) via the on-fabric look-up = un-normalised weight per "
     "key; Z = per-row total, R = 1/Z.\nrow = query, col = key.",
     [F_EA, F_EB, F_Z, F_R]),
    ("5 - attention map (probabilities)",
     "The normalised attention: for each query (row), the fraction taken from "
     "each key (column) - where each patch looks.\nrow = query, col = key.",
     [F_PQA, F_PQB]),
    ("6 - value mix (output tokens)",
     "Output token = attention-weighted blend of the value patches, summed "
     "over all 256 keys.\nrow = token, col 0..63 = output feature.",
     [F_OA, F_OB]),
    ("7 - un-patchify  (OUTPUT)",
     "Each output token's 64 features scattered back to its 8x8 patch "
     "location, rebuilding a 128x128 frame.\nrow = image row, col = image col.",
     [F_OUTPUT]),
]
FLOW_MAXCOLS = 4


def compose_flow(ppm_dir: str, out: str) -> int:
    nrows = len(STAGES_FLOW)
    fig = plt.figure(figsize=(15, 2.45 * nrows))
    gs = fig.add_gridspec(nrows, FLOW_MAXCOLS + 1,
                          width_ratios=[1, 1, 1, 1, 3.2],
                          hspace=0.55, wspace=0.30,
                          left=0.015, right=0.985, top=0.955, bottom=0.02)
    for r, (label, expl, panels) in enumerate(STAGES_FLOW):
        for c, (fname, mode) in enumerate(panels):
            img = load(os.path.join(ppm_dir, fname))
            ax = fig.add_subplot(gs[r, c])
            ax.set_aspect("equal")
            _show(ax, img, mode)
        tax = fig.add_subplot(gs[r, FLOW_MAXCOLS]); tax.axis("off")
        tax.text(0.0, 0.92, label, ha="left", va="top", fontsize=12,
                 fontweight="bold", color="#1a3b6b", transform=tax.transAxes)
        tax.text(0.0, 0.66, expl, ha="left", va="top", fontsize=9.5,
                 color="black", wrap=True, transform=tax.transAxes)
        if r < nrows - 1:
            tax.annotate("", xy=(0.02, -0.02), xytext=(0.02, 0.18),
                         xycoords="axes fraction",
                         arrowprops=dict(arrowstyle="-|>", color="#1a3b6b",
                                         lw=1.6))
    fig.suptitle("attention_self -- on-fabric ViT 8x8-patch self-attention: "
                 "the 128x128 data plane at each stage",
                 fontsize=14, fontweight="bold", y=0.985)
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ppm_dir")
    ap.add_argument("--out", default=None)
    ap.add_argument("--style", choices=["landscape", "flow"],
                    default="landscape")
    args = ap.parse_args()
    out = args.out or (f"flow_{'landscape' if args.style == 'landscape' else 'diagram'}.png")
    if args.style == "landscape":
        return compose_landscape(args.ppm_dir, out)
    return compose_flow(args.ppm_dir, out)


if __name__ == "__main__":
    sys.exit(main())
