"""Scene 2 - The subject: a (Pop) cat. Ends by sliding the cat left, ready to be
captured in Scene 3 (carryover)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import Scene, VGroup, Text, FadeIn, FadeOut, LaggedStart, UP, DOWN
from common.layout import make_cat, CAT_CENTER, CAT_LEFT
from common.palette import FG, MUTED, SANS, caption


class TheImage(Scene):
    def construct(self):
        intro = Text("Computer vision begins with an image.", font=SANS,
                     font_size=34, color=FG).move_to(UP * 2.4)
        cat = make_cat().move_to(CAT_CENTER)
        label = Text("an image", font=SANS, font_size=24, color=MUTED).next_to(cat, DOWN, buff=0.25)

        self.play(FadeIn(intro), run_time=0.7)
        self.play(LaggedStart(*[FadeIn(c) for c in cat.cell_list],
                              lag_ratio=0.002, run_time=1.4), FadeIn(label))
        cap = caption("Where does it go, and how is it processed?")
        self.play(FadeIn(cap), run_time=0.6)
        self.wait(1.2)

        # carry the cat into Scene 3 (slide left); drop the text
        self.play(FadeOut(VGroup(intro, label, cap)),
                  cat.animate.move_to(CAT_LEFT), run_time=0.9)
        self.wait(0.2)
