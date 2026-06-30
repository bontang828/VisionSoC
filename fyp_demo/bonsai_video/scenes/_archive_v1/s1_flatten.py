"""S1 - The tension: conventional vector hardware flattens a 2D image to 1D,
pulling spatial neighbours apart."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Square, Text, Brace, FadeIn, FadeOut, Create, Indicate,
    LEFT, RIGHT, UP, DOWN,
)
from common.assets import cat_image
from common.pixel_grid import PixelGrid
from common.palette import FG, MUTED, TEAL, AMBER, BLUE, SANS, MONO, caption

N = 12          # smaller grid so the 1D flatten is legible
CELL = 0.34


class FlattenProblem(Scene):
    def construct(self):
        img = cat_image(N)

        grid = PixelGrid(img, cell_size=CELL).move_to(LEFT * 3.6 + UP * 0.4)
        gl = Text("a 128 x 128 image", font=SANS, font_size=22, color=MUTED)
        gl.next_to(grid, UP, buff=0.18)
        self.play(FadeIn(grid), FadeIn(gl), run_time=0.9)

        # two vertically-adjacent pixels - neighbours in 2D
        r, c = 4, 6
        hi_a = Square(CELL).move_to(grid.cell_center(r, c)).set_stroke(AMBER, 4).set_fill(opacity=0)
        hi_b = Square(CELL).move_to(grid.cell_center(r + 1, c)).set_stroke(TEAL, 4).set_fill(opacity=0)
        adj = Text("vertical neighbours", font=SANS, font_size=20, color=FG)
        adj.next_to(grid, DOWN, buff=0.2)
        self.play(Create(hi_a), Create(hi_b), FadeIn(adj))
        self.play(Indicate(hi_a, color=AMBER), Indicate(hi_b, color=TEAL))

        # flatten the first 3 rows into a 1D strip on the right
        K = 3
        strip_cell = 0.30
        start = RIGHT * 0.2 + DOWN * 1.2
        targets = []
        for rr in range(K):
            for cc in range(N):
                idx = rr * N + cc
                pos = start + RIGHT * (idx * strip_cell)
                targets.append((grid.cell(rr, cc), pos))

        strip_label = Text("conventional 1D vector register", font=MONO,
                           font_size=22, color=BLUE).move_to(UP * 1.7 + RIGHT * 1.2)
        self.play(FadeIn(strip_label), run_time=0.6)

        anims = [cellmob.animate.move_to(pos).scale(strip_cell / CELL)
                 for cellmob, pos in targets]
        # fade the rows we are not flattening
        rest = [grid.cell(rr, cc) for rr in range(K, N) for cc in range(N)]
        self.play(*anims, *[m.animate.set_opacity(0.12) for m in rest], run_time=1.6)

        ellipsis = Text("...", font=MONO, font_size=30, color=BLUE)
        ellipsis.next_to(targets[-1][0], RIGHT, buff=0.2)
        self.play(FadeIn(ellipsis))

        # the two neighbours are now N cells apart
        a_cell = grid.cell(r, c)            # was at (r,c)
        # they were not in the first 3 rows (r=4); show the stride concept instead
        b0 = targets[0][0]                  # row 0 col 0
        b1 = targets[N][0]                  # row 1 col 0 (one full row later)
        hb = Square(strip_cell).move_to(b0.get_center()).set_stroke(AMBER, 4).set_fill(opacity=0)
        hb2 = Square(strip_cell).move_to(b1.get_center()).set_stroke(TEAL, 4).set_fill(opacity=0)
        self.play(Create(hb), Create(hb2))
        brace = Brace(VGroup(hb, hb2), DOWN, color=FG)
        btxt = brace.get_text("stride = 128").set_color(AMBER)
        btxt.scale(0.7)
        self.play(Create(brace), FadeIn(btxt))

        cap = caption("Conventional SIMD flattens the image to 1D - neighbours drift apart.")
        self.play(FadeIn(cap), run_time=0.7)
        self.wait(1.0)

        q = Text("What if the register kept the image's shape?",
                 font=SANS, font_size=30, color=TEAL).move_to(DOWN * 0.2)
        self.play(
            FadeOut(VGroup(strip_label, ellipsis, hb, hb2, brace, btxt, adj, gl)),
            FadeOut(cap),
            run_time=0.7,
        )
        self.play(FadeIn(q, shift=UP * 0.2), run_time=0.8)
        self.wait(1.2)
        self.play(FadeOut(VGroup(grid, hi_a, hi_b, q)), run_time=0.7)
