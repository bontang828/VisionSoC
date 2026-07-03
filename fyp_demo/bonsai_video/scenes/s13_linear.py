"""Scene 13 - BONSAI Processor is scalable: silicon size grows linearly with
throughput. Data points are Bonsai Processor chips of growing size."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Text, Arrow, Line, FadeIn, FadeOut, Create, LaggedStart,
    LEFT, RIGHT, UP, DOWN,
)
from common.icons import chip
from common.palette import FG, MUTED, TEAL, PURPLE, SANS, caption


def _p(x, y):
    return np.array([x, y, 0.0])


class LinearScaling(Scene):
    def construct(self):
        title = Text("BONSAI Processor is Scalable", font=SANS, font_size=34,
                     color=FG, weight="BOLD").to_edge(UP, buff=0.55)
        self.play(FadeIn(title, shift=UP * 0.15), run_time=0.6)

        o = _p(-4.0, -2.4)
        xax = Arrow(o, o + RIGHT * 8.0, color=FG, buff=0, stroke_width=3,
                    max_tip_length_to_length_ratio=0.03)
        yax = Arrow(o, o + UP * 4.6, color=FG, buff=0, stroke_width=3,
                    max_tip_length_to_length_ratio=0.05)
        xl = Text("Throughput (bits/cycle)", font=SANS, font_size=22,
                  color=MUTED).next_to(xax, DOWN, buff=0.2)
        yl = Text("Silicon size (µm²)", font=SANS, font_size=22,
                  color=MUTED).rotate(np.pi / 2).next_to(yax, LEFT, buff=0.2)
        self.play(Create(xax), Create(yax), FadeIn(xl), FadeIn(yl), run_time=0.9)

        # arrowhead on the line: it keeps increasing
        line = Arrow(o, o + RIGHT * 7.0 + UP * 4.0, color=TEAL, buff=0,
                     stroke_width=5, max_tip_length_to_length_ratio=0.045)
        # data points: BONSAI Processor chips of growing size (labels auto-fit,
        # so the word grows with each box)
        sizes = (0.6, 0.85, 1.1, 1.35)
        chips = VGroup(*[
            chip("BONSAI\nProcessor", w=s, h=s * 0.8, color=PURPLE,
                 font_size=18, fill_opacity=0.4,
                 weight="BOLD").move_to(o + RIGHT * (1.5 * k) + UP * (6.0 / 7.0 * k))
            for k, s in zip(range(1, 5), sizes)
        ])
        self.play(Create(line), run_time=1.0)
        self.play(LaggedStart(*[FadeIn(c, scale=1.2) for c in chips],
                              lag_ratio=0.2), run_time=1.2)

        # "Linear" tag sits clear of the line (above-left of it)
        tag = Text("Linear", font=SANS, font_size=30, color=TEAL,
                   weight="BOLD").move_to(o + RIGHT * 3.4 + UP * 3.4)
        cap = caption("Throughput increases with linear silicon growth.")
        self.play(FadeIn(tag), FadeIn(cap), run_time=0.7)
        self.wait(1.8)
        self.play(FadeOut(VGroup(title, xax, yax, xl, yl, line, chips, tag, cap)),
                  run_time=0.7)
