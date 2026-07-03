"""Scene 3.8 - Contribution 3: evaluation.
Headline "Evaluation of a vector-instruction-based 2D image-plane processor",
then a checklist of what was evaluated. Closes the "Our Contributions"
interlude (3.6 - 3.8).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Text, Arrow, FadeIn, FadeOut, GrowArrow,
    UP, DOWN, LEFT, RIGHT,
)
from common.palette import FG, MUTED, TEAL, GREEN, SANS


def _p(x, y):
    return np.array([x, y, 0.0])


def kicker(step):
    return Text(f"CONTRIBUTION  {step} / 3", font=SANS, font_size=18,
                color=TEAL).to_corner(UP + LEFT, buff=0.55)


def bullet_item(text):
    d = Text("•", font=SANS, font_size=24, color=TEAL)
    t = Text(text, font=SANS, font_size=22, color=FG).next_to(d, RIGHT, buff=0.22)
    return VGroup(d, t)


def tick_item(text):
    tick = Text("✓", font=SANS, font_size=24, color=GREEN, weight="BOLD")
    t = Text(text, font=SANS, font_size=22, color=FG).next_to(tick, RIGHT, buff=0.22)
    return VGroup(tick, t)


class ContributionEval(Scene):
    SLOWDOWN = 1.5   # word-heavy card: extra reading time

    def construct(self):
        tag = kicker(3)
        title = Text("How we evaluate BONSAI effectiveness?", font=SANS,
                     font_size=32, color=FG, weight="BOLD",
                     t2c={"BONSAI effectiveness": TEAL}).to_edge(UP, buff=1.1)

        h1 = Text("Live demo kernels", font=SANS, font_size=25, color=FG,
                  weight="BOLD").move_to(_p(-3.6, 0.0))
        b1 = VGroup(
            bullet_item("Sobel"),
            bullet_item("Optical flow"),
            bullet_item("Matrix multiplication"),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.32)
        b1.next_to(h1, DOWN, buff=0.45).align_to(h1, LEFT).shift(RIGHT * 0.3)

        h2 = Text("Architectural performance analysis", font=SANS, font_size=25,
                  color=FG, weight="BOLD")
        if h2.width > 6.2:
            h2.scale_to_fit_width(6.2)
        h2.move_to(_p(3.6, 0.0))
        b2 = VGroup(
            tick_item("Simpler program"),
            tick_item("End-to-end kernel"),
            tick_item("Image never leaves sensor chip"),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.32)
        b2.next_to(h2, DOWN, buff=0.45).align_to(h2, LEFT).shift(RIGHT * 0.3)

        a1 = Arrow(title.get_bottom() + DOWN * 0.1, h1.get_top() + UP * 0.1,
                   color=TEAL, buff=0.15, stroke_width=3,
                   max_tip_length_to_length_ratio=0.08)
        a2 = Arrow(title.get_bottom() + DOWN * 0.1, h2.get_top() + UP * 0.1,
                   color=TEAL, buff=0.15, stroke_width=3,
                   max_tip_length_to_length_ratio=0.08)

        self.play(FadeIn(tag), run_time=0.4)
        self.play(FadeIn(title, shift=UP * 0.15), run_time=0.7)
        self.play(GrowArrow(a1), FadeIn(h1, shift=UP * 0.15), run_time=0.6)
        for b in b1:
            self.play(FadeIn(b, shift=RIGHT * 0.2), run_time=0.45)
        self.play(GrowArrow(a2), FadeIn(h2, shift=UP * 0.15), run_time=0.6)
        for b in b2:
            self.play(FadeIn(b, shift=RIGHT * 0.2), run_time=0.45)
        self.wait(1.6)

        self.play(FadeOut(VGroup(tag, title, a1, a2, h1, b1, h2, b2)), run_time=0.8)
