"""Scene 3.9 - Section divider: "How does it work?"
A short transition card between the contributions interlude (3.6 - 3.8) and the
architecture deep-dive (Scene 4 onward).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import Scene, VGroup, Line, Text, FadeIn, FadeOut, GrowFromCenter, UP, DOWN
from common.palette import FG, TEAL, SANS


class HowItWorks(Scene):
    def construct(self):
        title = Text("How does a BONSAI Processor work?", font=SANS, font_size=52,
                     color=FG, weight="BOLD")
        rule = Line(np.array([-1.6, 0, 0]), np.array([1.6, 0, 0]),
                    stroke_width=3).set_color(TEAL).next_to(title, DOWN, buff=0.35)

        self.play(FadeIn(title, shift=UP * 0.2), run_time=0.8)
        self.play(GrowFromCenter(rule), run_time=0.5)
        self.wait(1.3)
        self.play(FadeOut(VGroup(title, rule)), run_time=0.8)
