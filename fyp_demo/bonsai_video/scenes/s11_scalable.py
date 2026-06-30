"""Scene 11 - Scalability. Re-uses the SAME simplified register-file stack from
Scene 10 (no new object). A grid FADES IN within the register file, then the
file scales in width and height; cells multiply at a fixed pitch (boxes stretch
to fill), so resolution counts up from 128x128."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Line, Text, DoubleArrow, ValueTracker, always_redraw,
    ManimColor, FadeIn, FadeOut, ORIGIN, LEFT, RIGHT, UP, DOWN,
)
from common.arch import make_reg_file, RF_DX, RF_DY, RF_PLANES
from common.palette import FG, MUTED, TEAL, AMBER, BLUE, MONO, SANS, caption

PITCH = 0.25       # fixed cell size -> cell COUNT grows as the file scales
INIT_W = 2.0
GRID_DARK = ManimColor("#0a1730")   # dark grid lines, clear against the blue planes


def _v(p, x, y):
    return np.array([p[0] + x, p[1] + y, 0.0])


class Scalable(Scene):
    def construct(self):
        # carry-in: the SAME simplified stack from Scene 10
        stack = make_reg_file(size=INIT_W).move_to(ORIGIN)
        self.add(stack)
        front = stack.top
        self.wait(0.4)

        grid_op = ValueTracker(0.0)

        def make_grid():
            c = front.get_center()
            s = front.width
            op = grid_op.get_value()
            g = VGroup()
            n = max(1, int(round(s / PITCH)))
            for k in range(1, n):
                f = -s / 2 + s * k / n
                g.add(Line(_v(c, f, -s / 2), _v(c, f, s / 2)).set_stroke(GRID_DARK, 1.4, opacity=op))
                g.add(Line(_v(c, -s / 2, f), _v(c, s / 2, f)).set_stroke(GRID_DARK, 1.4, opacity=op))
            return g

        def make_arrows():
            c = front.get_center()
            s = front.width
            w = DoubleArrow(_v(c, -s / 2, -s / 2 - 0.35), _v(c, s / 2, -s / 2 - 0.35),
                            color=TEAL, buff=0, stroke_width=4, tip_length=0.18)
            wl = Text("width", font_size=18, color=TEAL).next_to(w, DOWN, buff=0.08)
            h = DoubleArrow(_v(c, -s / 2 - 0.35, -s / 2), _v(c, -s / 2 - 0.35, s / 2),
                            color=TEAL, buff=0, stroke_width=4, tip_length=0.18)
            hl = Text("height", font_size=18, color=TEAL).next_to(h, LEFT, buff=0.08)
            # third (depth) axis on the BOTTOM-LEFT, along the stacking diagonal
            scale = s / INIT_W
            dvec = np.array([RF_DX, RF_DY, 0.0]) * (RF_PLANES - 1) * scale
            p1 = _v(c, -s / 2, -s / 2)                    # front, bottom-left corner
            p2 = p1 - dvec                                # down-left toward the back planes
            d = DoubleArrow(p1, p2, color=AMBER, buff=0, stroke_width=4, tip_length=0.16)
            dl = Text("32 registers", font_size=18, color=AMBER).next_to(p2, DOWN + LEFT, buff=0.04)
            return VGroup(w, wl, h, hl, d, dl)

        grid = always_redraw(make_grid)
        arrows = always_redraw(make_arrows)
        readout = always_redraw(
            lambda: Text(f"{int(round(128 * front.width / INIT_W))} x "
                         f"{int(round(128 * front.width / INIT_W))}",
                         font=MONO, font_size=40, color=AMBER).move_to(RIGHT * 4.2 + UP * 0.5)
        )
        res_lbl = Text("pixels stored", font=SANS, font_size=20, color=MUTED).move_to(RIGHT * 4.2 + DOWN * 0.1)

        self.add(grid)
        # 1) fade the grid IN within the existing register file (no new object)
        self.play(grid_op.animate.set_value(0.45), run_time=0.8)
        self.play(FadeIn(arrows), FadeIn(readout), FadeIn(res_lbl), run_time=0.6)
        cap = caption("Scale in width and height - more cells, more pixels, same programming model.")
        self.play(FadeIn(cap), run_time=0.5)

        # 2) scale the file; cells multiply at fixed pitch (stretch to fill)
        self.play(stack.animate.scale(2.0), run_time=2.8)
        self.wait(1.4)

        grid.clear_updaters()
        arrows.clear_updaters()
        readout.clear_updaters()
        self.play(FadeOut(VGroup(stack, grid, arrows, readout, res_lbl, cap)), run_time=0.8)
