"""Scene 1 - Title: BONSAI, researchers, bonsai-tree logo."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Text, FadeIn, FadeOut, GrowFromEdge, DrawBorderThenFill,
    UP, DOWN,
)
from common.icons import bonsai_tree
from common.palette import FG, MUTED, TEAL, SANS


class Title(Scene):
    def construct(self):
        tree = bonsai_tree(scale=1.3).move_to(UP * 1.1)
        title = Text("BONSAI", font=SANS, font_size=72, color=FG, weight="BOLD")
        title.next_to(tree, DOWN, buff=0.5)
        sub = Text("A 2D RISC-V Vision Processor", font=SANS, font_size=30, color=TEAL)
        sub.next_to(title, DOWN, buff=0.25)
        author1 = Text("Bon Tang", font=SANS, font_size=21, color=FG)
        author1.next_to(sub, DOWN, buff=0.4)
        authors = Text("Nicholas Fry · Shinjeong Kim · "
                       "Andrew J. Davison · Paul H. J. Kelly",
                       font=SANS, font_size=20, color=MUTED)
        if authors.width > 12.5:
            authors.scale_to_fit_width(12.5)
        authors.next_to(author1, DOWN, buff=0.18)
        inst = Text("Imperial College London   ·   ECCV 2026", font=SANS,
                    font_size=20, color=MUTED).next_to(authors, DOWN, buff=0.2)

        self.play(DrawBorderThenFill(tree), run_time=1.6)
        self.play(FadeIn(title, shift=UP * 0.2), run_time=0.8)
        self.play(FadeIn(sub), run_time=0.5)
        self.play(FadeIn(author1), FadeIn(authors), FadeIn(inst), run_time=0.7)
        self.wait(1.8)
        self.play(FadeOut(VGroup(tree, title, sub, author1, authors, inst)), run_time=0.9)
