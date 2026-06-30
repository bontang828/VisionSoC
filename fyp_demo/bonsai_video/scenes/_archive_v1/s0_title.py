"""S0 - Title sting. A single pixel blossoms into a cat-grid behind the title."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Text, FadeIn, FadeOut, GrowFromCenter, LaggedStart,
    UP, DOWN,
)
from common.assets import cat_image
from common.pixel_grid import PixelGrid
from common.palette import FG, MUTED, TEAL, SANS

N = 16


class Title(Scene):
    def construct(self):
        grid = PixelGrid(cat_image(N), cell_size=0.22)
        grid.set_opacity(0.0)
        grid.move_to(UP * 0.3)

        # one pixel grows, then the rest bloom in
        first = grid.cell(N // 2, N // 2)
        self.play(GrowFromCenter(first.copy().set_opacity(1.0)), run_time=0.6)
        self.play(
            LaggedStart(*[c.animate.set_opacity(1.0) for c in grid.cell_list],
                        lag_ratio=0.01, run_time=1.6)
        )
        self.add(grid)

        title = Text("BONSAI", font=SANS, font_size=68, color=FG, weight="BOLD")
        title.next_to(grid, DOWN, buff=0.45)
        sub = Text("A 2D RISC-V Vision Processor", font=SANS, font_size=30, color=TEAL)
        sub.next_to(title, DOWN, buff=0.2)
        tag = Text("Imperial College London   ·   ECCV 2026",
                   font=SANS, font_size=22, color=MUTED)
        tag.next_to(sub, DOWN, buff=0.25)

        self.play(FadeIn(title, shift=UP * 0.2), run_time=0.8)
        self.play(FadeIn(sub), FadeIn(tag), run_time=0.7)
        self.wait(1.2)
        self.play(FadeOut(VGroup(grid, title, sub, tag)), run_time=0.8)
