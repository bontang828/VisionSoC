"""Scene 3.8 - Contribution 3: evaluation.
Headline "Evaluation of a vector-instruction-based 2D image-plane processor",
then a checklist of what was evaluated. Closes the "Our Contributions"
interlude (3.6 - 3.8).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import Scene, VGroup, Text, FadeIn, FadeOut, UP, DOWN, LEFT, RIGHT
from common.palette import FG, MUTED, TEAL, GREEN, SANS


def _p(x, y):
    return np.array([x, y, 0.0])


def kicker(step):
    return Text(f"CONTRIBUTION  {step} / 3", font=SANS, font_size=18,
                color=TEAL).to_corner(UP + LEFT, buff=0.55)


def check_item(text):
    tick = Text("✓", font=SANS, font_size=28, color=GREEN)
    t = Text(text, font=SANS, font_size=26, color=FG).next_to(tick, RIGHT, buff=0.28)
    return VGroup(tick, t)


class ContributionEval(Scene):
    def construct(self):
        tag = kicker(3)
        t1 = Text("Evaluation of Bonsai", font=SANS,
                  font_size=30, color=FG)
        t2 = Text("Image Plane Vector Processor", font=SANS, font_size=30, color=FG)
        title = VGroup(t1, t2).arrange(DOWN, buff=0.12).to_edge(UP, buff=0.9)

        checks = VGroup(
            check_item("Demo kernels: Sobel, optical flow, matrix multiplication"),
            check_item("Architectural performance analysis"),
            check_item("Whole image as one instruction"),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.55).move_to(_p(0, -0.7))

        self.play(FadeIn(tag), run_time=0.4)
        self.play(FadeIn(title, shift=UP * 0.15), run_time=0.7)
        for c in checks:
            self.play(FadeIn(c, shift=RIGHT * 0.2), run_time=0.5)
        self.wait(1.6)

        self.play(FadeOut(VGroup(tag, title, checks)), run_time=0.8)
