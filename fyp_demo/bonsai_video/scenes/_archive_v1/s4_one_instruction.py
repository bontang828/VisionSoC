"""S4 - One instruction, the whole image. vadd.vv brightens all 16,384 pixels
at once, broadcast across 128 hardware rows."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Rectangle, Text, Integer, ValueTracker,
    FadeIn, FadeOut, Write, Create, rgb_to_color,
    AnimationGroup, LaggedStart,
    LEFT, RIGHT, UP, DOWN,
)
from common.assets import cat_image
from common.pixel_grid import PixelGrid
from common.palette import FG, MUTED, TEAL, AMBER, BLUE, GREEN, MONO, SANS, caption

N = 20
CELL = 0.20


class OneInstrWholeImage(Scene):
    def construct(self):
        img = cat_image(N)
        bright = np.clip(img * 2.0, 0, 1)  # vadd.vv v8,v8,v8 == doubling

        code = Text("vadd.vv v8, v8, v8", font=MONO, font_size=34, color=FG,
                    t2c={"vadd.vv": BLUE, "v8": TEAL}).move_to(UP * 3.2)

        grid = PixelGrid(img, cell_size=CELL).move_to(LEFT * 2.7 + DOWN * 0.3)
        reg_lbl = Text("v8", font=MONO, font_size=22, color=TEAL).next_to(grid, UP, buff=0.15)
        self.play(Write(code), FadeIn(grid), FadeIn(reg_lbl), run_time=1.0)

        # sweep bar travelling left -> right; cells brighten as it passes
        bar = Rectangle(width=CELL, height=CELL * N + 0.1, stroke_width=0)
        bar.set_fill(TEAL, opacity=0.35).move_to(grid.cell(0, 0).get_center() + LEFT * CELL)

        col_anims = []
        for c in range(N):
            col_cells = [grid.cell(r, c) for r in range(N)]
            targets = [rgb_to_color([bright[r, c]] * 3) for r in range(N)]
            col_anims.append(AnimationGroup(
                *[cell.animate.set_fill(col, opacity=1.0)
                  for cell, col in zip(col_cells, targets)]
            ))

        self.add(bar)
        self.play(
            bar.animate.move_to(grid.cell(0, N - 1).get_center() + RIGHT * CELL),
            LaggedStart(*col_anims, lag_ratio=0.5),
            run_time=2.4,
        )
        self.play(FadeOut(bar), run_time=0.3)

        # comparison panel: BONSAI 1 instr vs scalar CPU 16,384 ops
        panel = VGroup()
        b_row = VGroup(
            Text("BONSAI", font=SANS, font_size=24, color=TEAL),
            Text("1 instruction", font=SANS, font_size=24, color=FG),
        ).arrange(RIGHT, buff=0.3)
        s_lbl = Text("scalar CPU ops:", font=SANS, font_size=24, color=MUTED)
        vt = ValueTracker(0)
        s_num = Integer(0, color=AMBER, font_size=28)
        s_num.add_updater(lambda m: m.set_value(int(vt.get_value())))
        s_row = VGroup(s_lbl, s_num).arrange(RIGHT, buff=0.25)
        panel = VGroup(b_row, s_row).arrange(DOWN, buff=0.5, aligned_edge=LEFT)
        panel.move_to(RIGHT * 3.6 + DOWN * 0.3)

        self.play(FadeIn(b_row), FadeIn(s_row), run_time=0.6)
        self.play(vt.animate.set_value(16384), run_time=1.8)
        s_num.clear_updaters()
        s_num.set_value(16384)

        cap = caption("Every vector instruction is broadcast across 128 hardware rows.")
        self.play(FadeIn(cap), run_time=0.8)
        self.wait(1.4)
        self.play(FadeOut(VGroup(code, grid, reg_lbl, panel, cap)), run_time=0.7)
