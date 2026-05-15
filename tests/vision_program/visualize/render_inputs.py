#!/usr/bin/env python3
"""
render_inputs.py - Mirror of render_digit() in cnn_digit.c, in Python.

The CNN demo procedurally renders 10 stylised digit patterns at
runtime; until a CNN run is completed with grid_input_d dumps in the
run.log, this script lets you inspect what the input frames actually
look like by reproducing the same drawing logic in Python and saving
a 10-digit grid as a PNG.

Usage:

    python3 tests/vision_program/visualize/render_inputs.py \
        --out fyp_doc/cnn_digit_inputs.png

Optionally pair with the existing visualize.py output (which renders
the LUT and 10 template W_d edge-maps) to see the full pipeline:
input -> conv+ReLU+LUT -> binary edge map -> per-class score.
"""

import argparse
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROWS = 128
COLS = 128
BRIGHT = 80
DARK = 0

DIGIT_TOP = 16
DIGIT_BOTTOM = 112
DIGIT_LEFT = 16
DIGIT_RIGHT = 112
DIGIT_MID = 64
BAR_THICK = 10


def fill_rect(img, r0, r1, c0, c1, v):
    r0 = max(r0, 0); c0 = max(c0, 0)
    r1 = min(r1, ROWS); c1 = min(c1, COLS)
    img[r0:r1, c0:c1] = v


def draw_oval_ring(img, rcenter, ccenter, a_outer, b_outer, a_inner, b_inner):
    for r in range(ROWS):
        for c in range(COLS):
            dy = r - rcenter
            dx = c - ccenter
            outer = dx*dx*b_outer*b_outer + dy*dy*a_outer*a_outer
            outer_lim = a_outer*a_outer*b_outer*b_outer
            inner = dx*dx*b_inner*b_inner + dy*dy*a_inner*a_inner
            inner_lim = a_inner*a_inner*b_inner*b_inner
            if outer <= outer_lim and inner >= inner_lim:
                img[r, c] = BRIGHT


def render_digit(d):
    img = np.full((ROWS, COLS), DARK, dtype=np.int16)

    top = DIGIT_TOP
    bot = DIGIT_BOTTOM
    left = DIGIT_LEFT
    right = DIGIT_RIGHT
    mid = DIGIT_MID
    bt = BAR_THICK

    if d == 0:
        draw_oval_ring(img, mid, mid, 48, 40, 36, 30)
    elif d == 1:
        fill_rect(img, top, bot, mid - bt//2, mid + bt//2, BRIGHT)
    elif d == 2:
        fill_rect(img, top, top + bt, left, right, BRIGHT)
        fill_rect(img, bot - bt, bot, left, right, BRIGHT)
        for r in range(top + bt, bot - bt):
            c = right - bt - ((r - (top + bt)) * (right - left - bt)) // (bot - bt - top - bt)
            fill_rect(img, r, r + 1, c, c + bt, BRIGHT)
    elif d == 3:
        fill_rect(img, top, top + bt, left, right, BRIGHT)
        fill_rect(img, mid - bt//2, mid + bt//2, left, right, BRIGHT)
        fill_rect(img, bot - bt, bot, left, right, BRIGHT)
        fill_rect(img, top, bot, right - bt, right, BRIGHT)
    elif d == 4:
        fill_rect(img, top, mid + bt//2, left, left + bt, BRIGHT)
        fill_rect(img, mid - bt//2, mid + bt//2, left, right, BRIGHT)
        fill_rect(img, top, bot, right - bt, right, BRIGHT)
    elif d == 5:
        fill_rect(img, top, top + bt, left, right, BRIGHT)
        fill_rect(img, top, mid + bt//2, left, left + bt, BRIGHT)
        fill_rect(img, mid - bt//2, mid + bt//2, left, right, BRIGHT)
        fill_rect(img, mid - bt//2, bot, right - bt, right, BRIGHT)
        fill_rect(img, bot - bt, bot, left, right, BRIGHT)
    elif d == 6:
        fill_rect(img, top, top + bt, left, right, BRIGHT)
        fill_rect(img, top, bot, left, left + bt, BRIGHT)
        fill_rect(img, mid - bt//2, mid + bt//2, left, right, BRIGHT)
        fill_rect(img, mid - bt//2, bot, right - bt, right, BRIGHT)
        fill_rect(img, bot - bt, bot, left, right, BRIGHT)
    elif d == 7:
        fill_rect(img, top, top + bt, left, right, BRIGHT)
        for r in range(top + bt, bot):
            c = right - bt - ((r - (top + bt)) * (right - left - bt)) // (bot - top - bt)
            fill_rect(img, r, r + 1, c, c + bt, BRIGHT)
    elif d == 8:
        draw_oval_ring(img, mid, mid, 48, 40, 36, 30)
        fill_rect(img, mid - bt//2, mid + bt//2, left + 6, right - 6, BRIGHT)
    elif d == 9:
        draw_oval_ring(img, top + 32, mid, 36, 32, 24, 20)
        fill_rect(img, top + 32, bot, right - bt, right, BRIGHT)
        fill_rect(img, bot - bt, bot, left, right, BRIGHT)

    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="cnn_digit_inputs.png")
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()

    fig, axes = plt.subplots(2, 5, figsize=(15, 6.5))
    axes = axes.flatten()

    for d in range(10):
        img = render_digit(d)
        ax = axes[d]
        ax.imshow(img, cmap="gray", vmin=0, vmax=BRIGHT,
                  interpolation="nearest")
        ax.set_title(f"input '{d}'", fontsize=11)
        ax.set_xticks([0, 64, 127])
        ax.set_yticks([0, 64, 127])

    fig.suptitle("cnn_digit: 10 synthetic input patterns (128x128 i8)",
                 fontsize=13)
    fig.tight_layout()
    fig.savefig(args.out, dpi=200, bbox_inches="tight")
    print(f"[info] wrote {args.out}")
    if args.show:
        plt.show()
    plt.close(fig)


if __name__ == "__main__":
    main()
