"""S5 - The 2D superpower. A CSR flip rotates the sweep; diagonal banking makes
a column read cost the same as a row read, so transpose is free (A x B^T)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Square, Text, Arrow,
    FadeIn, FadeOut, Write, Create, Indicate, LaggedStart, GrowArrow,
    LEFT, RIGHT, UP, DOWN,
)
from common.assets import cat_image
from common.pixel_grid import PixelGrid
from common.palette import FG, MUTED, TEAL, AMBER, BLUE, MAGENTA, MONO, SANS, caption

N = 18
CELL = 0.22


class FreeTranspose(Scene):
    def construct(self):
        img = cat_image(N)
        grid = PixelGrid(img, cell_size=CELL).move_to(DOWN * 0.35)
        frame = Square(CELL * N).set_stroke(TEAL, 2).set_fill(opacity=0).move_to(grid)
        vlabel = Text("v8", font=MONO, font_size=24, color=TEAL).next_to(frame, UP, buff=0.16)
        self.play(FadeIn(grid), Create(frame), FadeIn(vlabel), run_time=0.9)

        code = Text("csrw 0x7c0, 0    vslideup.vi v8, v8, 1", font=MONO,
                    font_size=24, color=FG,
                    t2c={"csrw": BLUE, "0x7c0": AMBER, "vslideup.vi": BLUE}).move_to(UP * 3.2)
        self.play(Write(code), run_time=0.8)

        # horizontal mode: image shifts one column to the right
        hmode = Text("horizontal mode  ->  shift a column", font=SANS, font_size=22,
                     color=TEAL).move_to(DOWN * 3.0)
        h_arrow = Arrow(LEFT * 0.6, RIGHT * 0.6, color=TEAL, stroke_width=5).next_to(frame, RIGHT, buff=0.4)
        self.play(FadeIn(hmode), GrowArrow(h_arrow))
        self.play(grid.animate.shift(RIGHT * CELL), run_time=0.6)
        self.play(grid.animate.shift(LEFT * CELL), run_time=0.4)
        self.play(FadeOut(VGroup(hmode, h_arrow)), run_time=0.3)

        # vertical mode: flip the CSR, image shifts one row down
        code2 = Text("csrw 0x7c0, 1    vslideup.vi v8, v8, 1", font=MONO,
                     font_size=24, color=FG,
                     t2c={"csrw": BLUE, "0x7c0": AMBER, "1": MAGENTA,
                          "vslideup.vi": BLUE})
        code2.move_to(code)
        self.play(FadeOut(code), FadeIn(code2), run_time=0.5)
        vmode = Text("vertical mode  ->  shift a row", font=SANS, font_size=22,
                     color=MAGENTA).move_to(DOWN * 3.0)
        v_arrow = Arrow(UP * 0.6, DOWN * 0.6, color=MAGENTA, stroke_width=5).next_to(frame, DOWN, buff=0.4)
        self.play(FadeIn(vmode), GrowArrow(v_arrow))
        self.play(grid.animate.shift(DOWN * CELL), run_time=0.6)
        self.play(grid.animate.shift(UP * CELL), run_time=0.4)
        self.play(FadeOut(VGroup(vmode, v_arrow, code2)), run_time=0.3)

        # the headline: diagonal banking -> free transpose
        diag = Text("diagonal banking:  column read = row read", font=SANS,
                    font_size=24, color=MAGENTA).move_to(UP * 3.15)
        self.play(Write(diag), run_time=0.8)
        diag_cells = [grid.cell(i, i) for i in range(N)]
        self.play(LaggedStart(*[Indicate(c, color=MAGENTA, scale_factor=1.2)
                                for c in diag_cells], lag_ratio=0.06, run_time=1.2))

        # transpose: every cell (r,c) -> (c,r)
        centers = {(r, c): grid.cell(r, c).get_center()
                   for r in range(N) for c in range(N)}
        move = [grid.cell(r, c).animate.move_to(centers[(c, r)])
                for r in range(N) for c in range(N)]
        vlabel_t = Text("v8 (transposed)", font=MONO, font_size=24, color=MAGENTA).move_to(vlabel)
        self.play(LaggedStart(*move, lag_ratio=0.0008, run_time=2.0),
                  FadeOut(vlabel), FadeIn(vlabel_t))

        tag = Text("transpose is free  ->  A x Bᵀ for attention", font=SANS,
                   font_size=24, color=AMBER).move_to(DOWN * 2.55)
        cap = caption("One config-register flip switches the sweep - and transpose costs nothing.")
        self.play(FadeIn(tag), FadeIn(cap), run_time=0.8)
        self.wait(1.5)
        self.play(FadeOut(VGroup(grid, frame, vlabel_t, diag, tag, cap)), run_time=0.7)
