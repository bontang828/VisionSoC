"""Scene 16 - The decoder processes many instructions (RISC-V RVV ISA)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Text, Arrow, FadeIn, FadeOut, GrowArrow, LaggedStart,
    LEFT, RIGHT, UP, DOWN,
)
from common.icons import chip
from common.palette import FG, MUTED, TEAL, AMBER, BLUE, SANS, MONO, caption


class DecoderMulti(Scene):
    def construct(self):
        dec = chip("Decoder", w=2.4, h=2.6, color=AMBER, font_size=26).move_to(RIGHT * 3.0)
        self.play(FadeIn(dec), run_time=0.6)

        names = ["vadd", "vmul", "vshift", "vgather"]
        blobs = VGroup(*[chip(n, w=1.6, h=0.6, color=BLUE, font_size=20) for n in names])
        blobs.arrange(DOWN, buff=0.3).move_to(LEFT * 3.5)
        arrows = VGroup(*[Arrow(b.get_right(), dec.get_left(), color=BLUE, buff=0.1,
                               stroke_width=3, max_tip_length_to_length_ratio=0.1) for b in blobs])

        self.play(LaggedStart(*[FadeIn(b, shift=RIGHT * 0.2) for b in blobs], lag_ratio=0.2, run_time=1.2))
        self.play(LaggedStart(*[GrowArrow(a) for a in arrows], lag_ratio=0.15, run_time=0.9))

        rvv = Text("Supports the RISC-V Vector (RVV) ISA", font=SANS, font_size=26, color=TEAL).to_edge(UP, buff=0.6)
        cap = caption("The decoder handles many vector instructions.")
        self.play(FadeIn(rvv), FadeIn(cap), run_time=0.7)
        self.wait(1.5)
        self.play(FadeOut(VGroup(dec, blobs, arrows, rvv, cap)), run_time=0.7)
