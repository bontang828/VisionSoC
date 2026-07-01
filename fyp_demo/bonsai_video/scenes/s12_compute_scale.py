"""Scene 12 - The compute lanes are scalable: duplicate one lane downwards."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import Scene, VGroup, Text, FadeIn, FadeOut, LaggedStart, UP, DOWN
from common.arch import compute_lane
from common.palette import FG, MUTED, GREEN, SANS, caption


class ComputeScale(Scene):
    def construct(self):
        title = Text("Compute Lanes", font=SANS, font_size=30, color=GREEN).to_edge(UP, buff=0.6)
        lane0 = compute_lane().move_to(UP * 1.7)
        self.play(FadeIn(title), FadeIn(lane0), run_time=0.9)

        copies = []
        opac = [0.85, 0.65, 0.45]
        for i, op in enumerate(opac, start=1):
            l = compute_lane().move_to(UP * 1.7 + DOWN * i * 1.0)
            l.set_opacity(op)
            copies.append(l)
        dots = Text("·  ·  ·", font=SANS, font_size=34, color=MUTED).move_to(UP * 1.7 + DOWN * 4.0)

        self.play(LaggedStart(*[FadeIn(l, shift=DOWN * 0.3) for l in copies],
                              lag_ratio=0.4, run_time=1.6))
        self.play(FadeIn(dots), run_time=0.4)
        cap = caption("Need more throughput?  Add more compute lanes.")
        self.play(FadeIn(cap), run_time=0.6)
        self.wait(1.4)
        self.play(FadeOut(VGroup(title, lane0, *copies, dots, cap)), run_time=0.7)
