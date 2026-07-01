"""Scene 4 - The near-sensor die: sensor + buffer + BONSAI on one die.
The cat is captured onto the sensor and read into the buffer; it stays in the
buffer (carried into Scene 5). Buffer/BONSAI blocks also carry over."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Text, Line, Arrow, DashedVMobject, RoundedRectangle,
    FadeIn, FadeOut, Create, GrowArrow, LEFT, RIGHT, UP, DOWN,
)
from common.layout import make_cat, CAT_LEFT, SENSOR_POS, BUFFER_POS, BONSAI_POS
from common.icons import camera_sensor, chip
from common.palette import FG, MUTED, TEAL, BLUE, GREEN, PURPLE, SANS, caption


def build_near_sensor_blocks():
    sensor = camera_sensor(size=1.0).move_to(SENSOR_POS)
    buffer = chip("Buffer", w=1.0, h=1.0, color=GREEN).move_to(BUFFER_POS)
    bonsai = chip("BONSAI", w=1.5, h=1.4, color=PURPLE).move_to(BONSAI_POS)
    a_sb = Arrow(sensor.get_right(), buffer.get_left(), color=GREEN, buff=0.06,
                 stroke_width=3, max_tip_length_to_length_ratio=0.22)
    a_bb = Arrow(buffer.get_right(), bonsai.get_left(), color=GREEN, buff=0.06,
                 stroke_width=3, max_tip_length_to_length_ratio=0.22)
    return sensor, buffer, bonsai, a_sb, a_bb


class NearSensorDie(Scene):
    def construct(self):
        cat = make_cat().move_to(CAT_LEFT)
        self.play(FadeIn(cat), run_time=0.5)

        sensor, buffer, bonsai, a_sb, a_bb = build_near_sensor_blocks()
        inner = VGroup(sensor, buffer, bonsai)
        die = RoundedRectangle(width=inner.width + 1.2, height=inner.height + 1.5,
                               corner_radius=0.15, stroke_color=TEAL, stroke_width=2).move_to(inner)
        die_d = DashedVMobject(die, num_dashes=64, dashed_ratio=0.55).set_stroke(TEAL, 2, opacity=0.8)
        die_lbl = Text("NEAR-SENSOR DIE", font_size=18, color=TEAL, weight="BOLD").move_to(die.get_top() + DOWN * 0.28)
        sensor_lbl = Text("sensor", font_size=15, color=BLUE).next_to(sensor, DOWN, buff=0.12)

        self.play(Create(die_d), FadeIn(die_lbl), run_time=0.8)
        self.play(FadeIn(sensor), FadeIn(sensor_lbl), FadeIn(buffer), FadeIn(bonsai), run_time=0.7)
        self.play(GrowArrow(a_sb), GrowArrow(a_bb), run_time=0.5)

        # capture: cat scales onto the sensor, then is read into the buffer
        l1 = Line(cat.get_corner(UP + RIGHT), sensor.get_left(), stroke_width=2).set_stroke(TEAL, opacity=0.8)
        l2 = Line(cat.get_corner(DOWN + RIGHT), sensor.get_left(), stroke_width=2).set_stroke(TEAL, opacity=0.8)
        self.play(Create(l1), Create(l2), run_time=0.6)
        self.play(FadeOut(VGroup(l1, l2)),
                  cat.animate.scale_to_fit_width(0.95).move_to(sensor.get_center()), run_time=0.8)
        self.play(cat.animate.scale_to_fit_width(0.8).move_to(BUFFER_POS), run_time=0.7)

        cap = caption("The near-sensor die: the image is captured and kept on-chip, beside the sensor.")
        self.play(FadeIn(cap), run_time=0.7)
        self.wait(1.4)

        # carry out: keep buffer/BONSAI (+arrow) and the cat-in-buffer for Scene 5
        self.play(FadeOut(VGroup(sensor_lbl, die_d, die_lbl, sensor, a_sb, cap)), run_time=0.7)
        self.wait(0.2)
