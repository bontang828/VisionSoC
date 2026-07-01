"""Scene 13 - Scalable, with linear resource use (a simple linear plot)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Text, Arrow, Line, Dot, FadeIn, FadeOut, Create, GrowArrow,
    LEFT, RIGHT, UP, DOWN,
)
from common.palette import FG, MUTED, TEAL, AMBER, GREEN, SANS, caption


def _p(x, y):
    return np.array([x, y, 0.0])


class LinearScaling(Scene):
    def construct(self):
        o = _p(-4.0, -2.2)
        xax = Arrow(o, o + RIGHT * 8.0, color=FG, buff=0, stroke_width=3,
                    max_tip_length_to_length_ratio=0.03)
        yax = Arrow(o, o + UP * 4.6, color=FG, buff=0, stroke_width=3,
                    max_tip_length_to_length_ratio=0.05)
        xl = Text("compute lanes", font=SANS, font_size=22, color=MUTED).next_to(xax, DOWN, buff=0.2)
        yl = Text("resource / area", font=SANS, font_size=22, color=MUTED).rotate(np.pi / 2).next_to(yax, LEFT, buff=0.2)
        self.play(Create(xax), Create(yax), FadeIn(xl), FadeIn(yl), run_time=0.9)

        line = Line(o, o + RIGHT * 7.0 + UP * 4.0, color=TEAL, stroke_width=5)
        dots = VGroup(*[Dot(o + RIGHT * (1.4 * k) + UP * (0.8 * k), color=AMBER, radius=0.07)
                        for k in range(1, 5)])
        self.play(Create(line), run_time=1.0)
        self.play(FadeIn(dots), run_time=0.5)

        tag = Text("linear", font=SANS, font_size=30, color=TEAL).move_to(o + RIGHT * 5.2 + UP * 3.2)
        cap = caption("Scalable - resource use grows linearly with the number of lanes.")
        self.play(FadeIn(tag), FadeIn(cap), run_time=0.7)
        self.wait(1.5)
        self.play(FadeOut(VGroup(xax, yax, xl, yl, line, dots, tag, cap)), run_time=0.7)
