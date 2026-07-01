"""Scene 20 - Zoom out to the near-sensor die; two channels leave it:
a data channel (to the buffer) and an instruction channel (to BONSAI)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Text, Arrow, DashedVMobject, RoundedRectangle,
    FadeIn, FadeOut, Create, GrowArrow, LEFT, RIGHT, UP, DOWN,
)
from common.icons import camera_sensor, chip
from common.palette import FG, MUTED, TEAL, AMBER, BLUE, GREEN, PURPLE, SANS, caption


class DieChannels(Scene):
    def construct(self):
        sensor = camera_sensor(size=1.0).move_to(LEFT * 4.0 + UP * 0.2)
        buffer = chip("Buffer", w=1.1, h=1.0, color=GREEN).move_to(LEFT * 1.8 + UP * 0.2)
        bonsai = chip("BONSAI", w=1.7, h=1.5, color=PURPLE).move_to(RIGHT * 0.6 + UP * 0.2)
        inner = VGroup(sensor, buffer, bonsai)
        die = RoundedRectangle(width=inner.width + 1.4, height=inner.height + 1.6,
                               corner_radius=0.15, stroke_color=TEAL, stroke_width=2).move_to(inner)
        die_d = DashedVMobject(die, num_dashes=72, dashed_ratio=0.55).set_stroke(TEAL, 2, opacity=0.8)
        die_lbl = Text("NEAR-SENSOR DIE", font_size=18, color=TEAL, weight="BOLD").move_to(die.get_top() + DOWN * 0.28)

        self.play(Create(die_d), FadeIn(die_lbl), FadeIn(inner), run_time=1.0)

        out_x = 5.6
        # instruction channel out of BONSAI (upper)
        instr = Arrow(bonsai.get_right(), np.array([out_x, bonsai.get_center()[1] + 0.4, 0]),
                      color=BLUE, buff=0.1, stroke_width=4, max_tip_length_to_length_ratio=0.08)
        instr_l = Text("instruction channel", font_size=18, color=BLUE).next_to(instr, UP, buff=0.1)
        # data channel out of the buffer (lower)
        data = Arrow(buffer.get_bottom(), np.array([out_x, -1.7, 0]),
                     color=GREEN, buff=0.1, stroke_width=4, max_tip_length_to_length_ratio=0.06)
        data_l = Text("data channel", font_size=18, color=GREEN).next_to(data.get_end(), UP, buff=0.1)

        self.play(GrowArrow(instr), FadeIn(instr_l), run_time=0.7)
        self.play(GrowArrow(data), FadeIn(data_l), run_time=0.7)
        cap = caption("Two channels leave the die: data (to the buffer) and instructions (to BONSAI).")
        self.play(FadeIn(cap), run_time=0.7)
        self.wait(1.6)
        self.play(FadeOut(VGroup(die_d, die_lbl, inner, instr, instr_l, data, data_l, cap)), run_time=0.8)
