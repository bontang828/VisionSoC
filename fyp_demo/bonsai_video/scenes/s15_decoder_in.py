"""Scene 15 - Zoom into the decoder; instructions come in (one arrow L->R)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import Scene, VGroup, Text, Arrow, FadeIn, FadeOut, GrowArrow, LEFT, RIGHT, UP, DOWN
from common.icons import chip
from common.palette import FG, MUTED, AMBER, BLUE, SANS, caption


class DecoderIn(Scene):
    def construct(self):
        dec = chip("Decoder", w=2.4, h=1.9, color=AMBER, font_size=26).move_to(RIGHT * 1.0)
        self.play(FadeIn(dec), run_time=0.7)
        arrow = Arrow(LEFT * 5.5 + dec.get_center()[1] * UP, dec.get_left(),
                      color=BLUE, buff=0.15, stroke_width=5, max_tip_length_to_length_ratio=0.08)
        lbl = Text("instructions in", font=SANS, font_size=24, color=BLUE).next_to(arrow, UP, buff=0.15)
        self.play(GrowArrow(arrow), FadeIn(lbl), run_time=0.9)
        cap = caption("Zoom into the decoder - instructions arrive here.")
        self.play(FadeIn(cap), run_time=0.6)
        self.wait(1.4)
        self.play(FadeOut(VGroup(dec, arrow, lbl, cap)), run_time=0.7)
