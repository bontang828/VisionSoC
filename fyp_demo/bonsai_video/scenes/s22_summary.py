"""Scene 22 - Summary (the very last scene). A keyword-highlighted statement,
a three-way comparison table revealed row by row, a golden highlight on the
BONSAI (near-sensor) column, and the ending statement."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Text, Line, RoundedRectangle, ManimColor,
    FadeIn, FadeOut, Create, LaggedStart, Indicate,
    UP, DOWN, LEFT, RIGHT,
)
from common.palette import FG, MUTED, TEAL, GREEN, RED, PURPLE, SANS

GOLD = ManimColor("#f2c14e")

# table geometry
COL_X = (-2.6, 0.6, 3.8)      # On-sensor | Near-sensor (BONSAI) | Conventional
LABEL_X = -6.55               # left edge of the row-label column
HEAD_Y = 1.05
ROW_Y = (-0.15, -1.15, -2.15)
CELL_W = 2.9


def _p(x, y):
    return np.array([x, y, 0.0])


def rich_line(pieces, size=27):
    """A line of text pieces: (text, color, bold)."""
    g = VGroup(*[
        Text(t, font=SANS, font_size=size, color=c,
             weight="BOLD" if b else "NORMAL")
        for t, c, b in pieces
    ])
    g.arrange(RIGHT, buff=0.15, aligned_edge=DOWN)
    return g


def cell(ok, desc):
    mark = Text("✓" if ok else "✗", font=SANS, font_size=30,
                color=GREEN if ok else RED, weight="BOLD")
    d = Text(desc, font=SANS, font_size=17, color=MUTED)
    if d.width > CELL_W:
        d.scale_to_fit_width(CELL_W)
    return VGroup(mark, d).arrange(DOWN, buff=0.1)


class Summary(Scene):
    def construct(self):
        heading = Text("Summary", font=SANS, font_size=40, color=FG,
                       weight="BOLD").to_corner(UP + LEFT, buff=0.5)
        self.play(FadeIn(heading, shift=DOWN * 0.15), run_time=0.6)
        self.wait(0.3)

        # --- first blob: what BONSAI prioritises ---
        b1l1 = rich_line([
            ("BONSAI prioritises", FG, False),
            ("data layout,", TEAL, True),
            ("spatial locality", TEAL, True),
            ("and", FG, False),
        ])
        b1l2 = rich_line([
            ("popular open-source standards", TEAL, True),
            ("instead of raw compute throughput.", FG, False),
        ])
        blob1 = VGroup(b1l1, b1l2).arrange(DOWN, buff=0.16).move_to(_p(0, 2.45))
        self.play(LaggedStart(FadeIn(b1l1, shift=UP * 0.1),
                              FadeIn(b1l2, shift=UP * 0.1),
                              lag_ratio=0.35), run_time=1.2)
        self.wait(1.2)

        # --- comparison table: headers + separator lines ---
        heads = VGroup(
            Text("On-sensor\n(SCAMP-5)", font=SANS, font_size=21, color=FG,
                 weight="BOLD", line_spacing=0.8),
            Text("Near-sensor\n(BONSAI)", font=SANS, font_size=21, color=PURPLE,
                 weight="BOLD", line_spacing=0.8),
            Text("Conventional\nprocessing", font=SANS, font_size=21, color=FG,
                 weight="BOLD", line_spacing=0.8),
        )
        for h, x in zip(heads, COL_X):
            h.move_to(_p(x, HEAD_Y))
        hline = Line(_p(-6.7, 0.5), _p(5.35, 0.5), color=MUTED, stroke_width=2)
        vline = Line(_p(-4.45, 1.6), _p(-4.45, -2.7), color=MUTED, stroke_width=2)
        self.play(Create(hline), Create(vline),
                  LaggedStart(*[FadeIn(h, shift=UP * 0.1) for h in heads],
                              lag_ratio=0.2), run_time=1.0)
        self.wait(0.4)

        # --- rows, one by one ---
        rows_spec = [
            ("CNN & Attention",
             [(False, "only CNN"), (True, "local & global"), (True, "local & global")]),
            ("Low bandwidth\noff-chip",
             [(True, "only results"), (True, "only results"), (False, "raw image")]),
            ("Low latency\n& power",
             [(True, "on-chip compute"), (True, "on-chip compute"), (False, "off-chip compute")]),
        ]
        table_rows = VGroup()
        for (label, cells_spec), y in zip(rows_spec, ROW_Y):
            lab = Text(label, font=SANS, font_size=20, color=FG,
                       weight="BOLD", line_spacing=0.8)
            if lab.width > 1.95:
                lab.scale_to_fit_width(1.95)
            lab.move_to(_p(LABEL_X, y), aligned_edge=LEFT)
            cells = VGroup(*[cell(ok, d).move_to(_p(x, y))
                             for (ok, d), x in zip(cells_spec, COL_X)])
            self.play(FadeIn(lab),
                      LaggedStart(*[FadeIn(c, shift=UP * 0.12) for c in cells],
                                  lag_ratio=0.25), run_time=0.9)
            self.wait(0.5)
            table_rows.add(VGroup(lab, cells))

        # --- golden highlight on the BONSAI column ---
        gold = RoundedRectangle(width=3.15, height=4.45, corner_radius=0.14,
                                stroke_color=GOLD, stroke_width=4)
        gold.set_fill(GOLD, 0.07).move_to(_p(COL_X[1], -0.55))
        self.play(Create(gold), run_time=0.9)
        self.play(Indicate(heads[1], scale_factor=1.15, color=GOLD), run_time=0.8)
        self.wait(0.5)

        # --- ending statement ---
        e1 = rich_line([
            ("Ready-to-deploy prototype", GOLD, True),
            ("with", FG, False),
            ("well-known ISA", GOLD, True),
            ("and", FG, False),
            ("portable codebase", GOLD, True),
        ], size=25)
        e2 = rich_line([
            ("for vision researchers to pick up", FG, False),
            ("now", GOLD, True),
            ("- no time cost to re-learn.", GOLD, True),
        ], size=23)
        ending = VGroup(e1, e2).arrange(DOWN, buff=0.14).move_to(_p(0, -3.3))
        self.play(FadeIn(ending, shift=UP * 0.15), run_time=1.0)
        self.wait(3.0)

        self.play(FadeOut(VGroup(heading, blob1, heads, hline, vline,
                                 table_rows, gold, ending)), run_time=1.0)
