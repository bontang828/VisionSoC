"""S8 - Closing card."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Text, FadeIn, FadeOut, LaggedStart, UP, DOWN,
)
from common.assets import cat_image
from common.pixel_grid import PixelGrid
from common.palette import FG, MUTED, TEAL, AMBER, SANS, MONO


class Closing(Scene):
    def construct(self):
        logo = PixelGrid(cat_image(14), cell_size=0.16).move_to(UP * 1.9)
        title = Text("BONSAI", font=SANS, font_size=56, color=FG, weight="BOLD")
        title.next_to(logo, DOWN, buff=0.3)
        one_liner = Text(
            "An image is one vector register - one RISC-V instruction per frame, "
            "transpose for free.",
            font=SANS, font_size=24, color=TEAL,
        )
        one_liner.next_to(title, DOWN, buff=0.3)
        if one_liner.width > 12:
            one_liner.scale_to_fit_width(12)

        authors = Text(
            "Bon Tang · Nicholas Fry · Shinjeong Kim · Andrew J. Davison · Paul H. J. Kelly",
            font=SANS, font_size=20, color=FG,
        )
        if authors.width > 12.5:
            authors.scale_to_fit_width(12.5)
        inst = Text("Imperial College London   ·   ECCV 2026", font=SANS,
                    font_size=22, color=MUTED)
        contact = Text("cbt22@ic.ac.uk", font=MONO, font_size=20, color=AMBER)
        bottom = VGroup(authors, inst, contact).arrange(DOWN, buff=0.18)
        bottom.next_to(one_liner, DOWN, buff=0.5)

        self.play(LaggedStart(*[FadeIn(c) for c in logo.cell_list],
                              lag_ratio=0.01, run_time=1.2))
        self.add(logo)
        self.play(FadeIn(title, shift=UP * 0.2), run_time=0.7)
        self.play(FadeIn(one_liner), run_time=0.6)
        self.play(FadeIn(bottom), run_time=0.8)
        self.wait(2.0)
        self.play(FadeOut(VGroup(logo, title, one_liner, bottom)), run_time=1.0)
