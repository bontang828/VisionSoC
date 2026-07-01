"""Scene 2 - The subject: an image (the cat). Ends by downscaling the cat and
parking it on the left, ready for the three-pipeline comparison (Scene 3.5)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import Scene, Group, Text, FadeIn, FadeOut, UP, DOWN
from common.layout import make_cat_photo, CAT_CENTER, CAT_SMALL_POS, CAT_SMALL_W
from common.palette import FG, MUTED, SANS, caption


class TheImage(Scene):
    def construct(self):
        intro = Text("Computer vision begins with an image.", font=SANS,
                     font_size=34, color=FG).move_to(UP * 2.4)
        cat = make_cat_photo(3.6).move_to(CAT_CENTER)
        label = Text("an image", font=SANS, font_size=24, color=MUTED).next_to(cat, DOWN, buff=0.25)

        self.play(FadeIn(intro), run_time=0.7)
        self.play(FadeIn(cat), FadeIn(label), run_time=0.9)
        cap = caption("Where does it go, and how is it processed?")
        self.play(FadeIn(cap), run_time=0.6)
        self.wait(1.2)

        # downscale the cat and move it to the left (carried into Scene 3.5)
        self.play(FadeOut(intro), FadeOut(label), FadeOut(cap),
                  cat.animate.scale(CAT_SMALL_W / 3.6).move_to(CAT_SMALL_POS), run_time=0.9)
        self.wait(0.2)
