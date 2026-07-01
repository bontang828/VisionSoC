"""Scene 21 - The two dies side by side: the near-sensor die and a scalar-core
SoC die, connected by the data and instruction channels."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Text, DoubleArrow, DashedVMobject, RoundedRectangle,
    FadeIn, FadeOut, Create, GrowArrow, LEFT, RIGHT, UP, DOWN,
)
from common.icons import camera_sensor, chip
from common.palette import FG, MUTED, TEAL, AMBER, BLUE, GREEN, PURPLE, SANS, caption


def dashed_die(mob, color, pad_w=0.7, pad_h=0.9):
    r = RoundedRectangle(width=mob.width + pad_w * 2, height=mob.height + pad_h * 2,
                         corner_radius=0.15, stroke_color=color, stroke_width=2).move_to(mob)
    return DashedVMobject(r, num_dashes=72, dashed_ratio=0.55).set_stroke(color, 2, opacity=0.8), r


class TwoDies(Scene):
    def construct(self):
        # near-sensor die (left)
        sensor = camera_sensor(size=0.7).move_to(LEFT * 5.4 + UP * 0.2)
        buffer = chip("Buffer", w=0.85, h=0.8, color=GREEN).move_to(LEFT * 4.0 + UP * 0.2)
        bonsai = chip("BONSAI", w=1.3, h=1.2, color=PURPLE).move_to(LEFT * 2.4 + UP * 0.2)
        near_inner = VGroup(sensor, buffer, bonsai)
        near_d, near_box = dashed_die(near_inner, TEAL)
        near_lbl = Text("NEAR-SENSOR DIE", font_size=16, color=TEAL, weight="BOLD").move_to(near_box.get_top() + DOWN * 0.25)

        # scalar-core SoC die (right)
        scalar = chip("Scalar Core", w=2.0, h=2.0, color=MUTED, font_size=22).move_to(RIGHT * 4.6 + UP * 0.2)
        soc_d, soc_box = dashed_die(scalar, MUTED, pad_w=0.6, pad_h=0.8)
        soc_lbl = Text("SoC DIE", font_size=16, color=MUTED, weight="BOLD").move_to(soc_box.get_top() + DOWN * 0.25)

        self.play(Create(near_d), FadeIn(near_lbl), FadeIn(near_inner), run_time=0.9)
        self.play(Create(soc_d), FadeIn(soc_lbl), FadeIn(scalar), run_time=0.7)

        # two channels between the dies
        instr = DoubleArrow(bonsai.get_right() + UP * 0.3, scalar.get_left() + UP * 0.3,
                            color=BLUE, buff=0.2, stroke_width=4, tip_length=0.18)
        instr_l = Text("instruction channel", font_size=16, color=BLUE).next_to(instr, UP, buff=0.08)
        data = DoubleArrow(buffer.get_bottom() + DOWN * 0.1, scalar.get_left() + DOWN * 0.7,
                           color=GREEN, buff=0.15, stroke_width=4, tip_length=0.18)
        data_l = Text("data channel", font_size=16, color=GREEN).next_to(data, DOWN, buff=0.08)

        self.play(GrowArrow(instr), FadeIn(instr_l), run_time=0.7)
        self.play(GrowArrow(data), FadeIn(data_l), run_time=0.7)
        cap = caption("Near-sensor die and scalar-core SoC die, linked by the data and "
                      "instruction channels.")
        self.play(FadeIn(cap), run_time=0.7)
        self.wait(1.8)
        self.play(FadeOut(VGroup(near_d, near_lbl, near_inner, soc_d, soc_lbl, scalar,
                                 instr, instr_l, data, data_l, cap)), run_time=0.8)
