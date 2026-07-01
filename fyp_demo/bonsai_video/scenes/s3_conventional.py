"""Scene 3 - The conventional pipeline (top of the screen).
The cat is captured onto the sensor, then travels sensor -> CPU -> GPU across
separate dies (high power). The pipeline is built with the SHARED
`conventional_pipeline()` object and LEFT ON SCREEN, so Scene 3.5 carries in the
exact same object (continuous, not a new one)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Text, Line, Arrow, DashedVMobject, RoundedRectangle,
    FadeIn, FadeOut, Create, Flash, LEFT, RIGHT, UP, DOWN,
)
from common.layout import make_cat, CAT_LEFT
from common.icons import camera_sensor, chip, lightning
from common.palette import FG, MUTED, TEAL, AMBER, BLUE, RED, SANS, caption

CONV_Y = 2.6
# pipeline names sit on the LEFT of each pipeline, at the row's level (per drawio)
CONV_LBL_POS = np.array([-4.6, CONV_Y, 0.0])
PIPE_X0 = -2.7    # x of the first block (sensor/grid) in every pipeline (right of the cat)


def _p(x, y):
    return np.array([x, y, 0.0])


def die_around(mob, color, pad_w=0.28, pad_h=0.28):
    """A dashed die outline around mob, with a small "Die" label at its top-left."""
    r = RoundedRectangle(width=mob.width + pad_w * 2, height=mob.height + pad_h * 2,
                         corner_radius=0.1, stroke_color=color, stroke_width=1.8).move_to(mob)
    d = DashedVMobject(r, num_dashes=22, dashed_ratio=0.55).set_stroke(color, 1.8, opacity=0.7)
    lbl = Text("silicon die", font=SANS, font_size=11, color=color)
    lbl.move_to(r.get_bottom() + UP * 0.15)
    return VGroup(d, lbl)


def conventional_pipeline():
    """Shared conventional pipeline (sensor|CPU|GPU on separate dies, high power).
    Used by BOTH Scene 3 and Scene 3.5 so the object is continuous."""
    sensor = camera_sensor(0.7).move_to(_p(PIPE_X0, CONV_Y))
    cpu = chip("CPU", w=1.0, h=0.85, color=AMBER).move_to(_p(PIPE_X0 + 2.5, CONV_Y))
    gpu = chip("GPU", w=1.1, h=0.9, color=RED).move_to(_p(PIPE_X0 + 5.0, CONV_Y))
    sensor_die = die_around(sensor, BLUE)
    cpu_die = die_around(cpu, AMBER)
    gpu_die = die_around(gpu, RED)
    a1 = Arrow(sensor.get_right(), cpu.get_left(), buff=0.35, stroke_width=3,
               max_tip_length_to_length_ratio=0.12).set_color_by_gradient(FG, RED)
    a2 = Arrow(cpu.get_right(), gpu.get_left(), buff=0.2, stroke_width=3,
               max_tip_length_to_length_ratio=0.12).set_color_by_gradient(FG, RED)
    bolt1 = lightning(0.5, AMBER).move_to(a1.get_center() + UP * 0.35)
    bolt2 = lightning(0.5, RED).move_to(a2.get_center() + UP * 0.35)
    e1 = Text("high power", font_size=12, color=AMBER).next_to(bolt1, UP, buff=0.03)
    e2 = Text("high power", font_size=12, color=RED).next_to(bolt2, UP, buff=0.03)
    g = VGroup(sensor_die, cpu_die, gpu_die, sensor, cpu, gpu, a1, a2, bolt1, bolt2, e1, e2)
    g.sensor = sensor
    g.sensor_die = sensor_die
    g.cpu, g.gpu, g.a1, g.a2 = cpu, gpu, a1, a2
    g.cpu_die, g.gpu_die = cpu_die, gpu_die
    g.rest = VGroup(cpu_die, gpu_die, cpu, gpu, a1, a2)
    g.bolt1, g.e1, g.bolt2, g.e2 = bolt1, e1, bolt2, e2
    # stage groups for one-by-one reveal (Scene 3.5)
    g.stage_sensor = VGroup(sensor, sensor_die)
    g.stage_cpu = VGroup(a1, cpu, cpu_die, bolt1, e1)
    g.stage_gpu = VGroup(a2, gpu, gpu_die, bolt2, e2)
    return g


def conv_label():
    # grey (same as the On-sensor label) so it doesn't clash with the red "Bad"
    # of the gradient axis, and lets the teal Near-sensor pipeline stand out
    return Text("Conventional\npipeline", font=SANS, font_size=19, color=MUTED,
                line_spacing=0.7).move_to(CONV_LBL_POS)


class ConventionalPipeline(Scene):
    def construct(self):
        cat = make_cat(n=24).move_to(CAT_LEFT)     # carried in from Scene 2
        self.add(cat)
        conv = conventional_pipeline()

        # capture: cat scales onto the sensor
        self.play(FadeIn(conv.sensor), FadeIn(conv.sensor_die), run_time=0.5)
        l1 = Line(cat.get_corner(UP + RIGHT), conv.sensor.get_left(), stroke_width=2).set_stroke(TEAL, opacity=0.8)
        l2 = Line(cat.get_corner(DOWN + RIGHT), conv.sensor.get_left(), stroke_width=2).set_stroke(TEAL, opacity=0.8)
        capA = caption("A camera sensor captures the scene.")
        self.play(Create(l1), Create(l2), FadeIn(capA), run_time=0.8)
        self.play(FadeOut(VGroup(l1, l2)),
                  cat.animate.scale_to_fit_width(0.75).move_to(conv.sensor.get_center()), run_time=0.8)

        # build the rest; cat travels sensor -> CPU -> GPU
        self.play(FadeOut(capA), FadeIn(conv.rest), run_time=0.7)
        self.play(cat.animate.move_to(conv.cpu.get_center()), FadeIn(conv.bolt1), FadeIn(conv.e1),
                  Flash(conv.a1.get_center(), color=AMBER, line_length=0.12), run_time=0.8)
        self.play(cat.animate.move_to(conv.gpu.get_center()), FadeIn(conv.bolt2), FadeIn(conv.e2),
                  Flash(conv.a2.get_center(), color=RED, line_length=0.12), run_time=0.8)

        label = conv_label()
        capC = caption("The whole image moves across separate chips - the transfer burns energy.")
        self.play(FadeIn(label), FadeIn(capC), run_time=0.7)
        self.wait(1.2)
        # keep the conventional pipeline + label; drop only the cat/caption
        self.play(FadeOut(cat), FadeOut(capC), run_time=0.6)
        self.wait(0.2)
