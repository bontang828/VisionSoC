"""Scene 3 - The conventional pipeline.
The cat is captured (it scales down onto the sensor), then that SAME cat image
travels sensor -> CPU -> GPU across separate dies. Lightning sits ON the arrows:
the data transfer burns energy."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Text, Line, Arrow, DashedVMobject, RoundedRectangle,
    FadeIn, FadeOut, Create, GrowArrow, Flash,
    LEFT, RIGHT, UP, DOWN,
)
from common.layout import make_cat, CAT_LEFT
from common.icons import camera_sensor, chip, lightning
from common.palette import FG, MUTED, TEAL, AMBER, BLUE, RED, SANS, caption

PIPE_Y = 1.9


def die_box(mob, color, label):
    r = RoundedRectangle(width=mob.width + 0.7, height=mob.height + 0.9,
                         corner_radius=0.12, stroke_color=color, stroke_width=2).move_to(mob)
    d = DashedVMobject(r, num_dashes=40, dashed_ratio=0.55).set_stroke(color, 2, opacity=0.7)
    t = Text(label, font_size=15, color=color).move_to(r.get_bottom() + UP * 0.2)
    return VGroup(d, t)


class ConventionalPipeline(Scene):
    def construct(self):
        cat = make_cat().move_to(CAT_LEFT)        # carried in from Scene 2
        self.add(cat)

        # ---- 3A: capture - the cat scales down onto the sensor -----------
        sensor = camera_sensor(size=1.4).move_to(RIGHT * 4.0 + UP * 0.35)
        sensor_lbl = Text("camera sensor", font_size=18, color=BLUE).next_to(sensor, DOWN, buff=0.2)
        self.play(FadeIn(sensor), FadeIn(sensor_lbl), run_time=0.6)
        l1 = Line(cat.get_corner(UP + RIGHT), sensor.get_left(), stroke_width=2).set_stroke(TEAL, opacity=0.8)
        l2 = Line(cat.get_corner(DOWN + RIGHT), sensor.get_left(), stroke_width=2).set_stroke(TEAL, opacity=0.8)
        capA = caption("A camera sensor captures the scene.")
        self.play(Create(l1), Create(l2), FadeIn(capA), run_time=0.8)
        self.play(FadeOut(VGroup(l1, l2)),
                  cat.animate.scale(0.30).move_to(sensor.get_center()), run_time=0.9)

        # ---- 3B: lift sensor (with the cat on it) to start the pipeline ---
        self.play(FadeOut(capA), run_time=0.3)
        pipe_start = LEFT * 4.6 + UP * PIPE_Y
        self.play(sensor.animate.move_to(pipe_start).scale(0.8),
                  cat.animate.move_to(pipe_start).scale(0.8),
                  sensor_lbl.animate.move_to(pipe_start + DOWN * 0.85).scale(0.85),
                  run_time=0.9)

        # ---- 3C: the cat travels sensor -> CPU -> GPU --------------------
        cpu = chip("CPU", w=1.6, h=1.2, color=AMBER).move_to(LEFT * 0.2 + UP * PIPE_Y)
        gpu = chip("GPU", w=1.8, h=1.3, color=RED).move_to(RIGHT * 4.2 + UP * PIPE_Y)
        cpu_die = die_box(cpu, AMBER, "die")
        gpu_die = die_box(gpu, RED, "die")
        a1 = Arrow(sensor.get_right(), cpu.get_left(), color=FG, buff=0.45, stroke_width=4,
                   max_tip_length_to_length_ratio=0.1)
        a2 = Arrow(cpu.get_right(), gpu.get_left(), color=FG, buff=0.45, stroke_width=4,
                   max_tip_length_to_length_ratio=0.1)
        self.play(FadeIn(cpu), FadeIn(gpu), Create(cpu_die), Create(gpu_die), run_time=0.8)
        self.play(GrowArrow(a1), GrowArrow(a2), run_time=0.6)

        bolt1 = lightning(scale=0.8, color=AMBER).move_to(a1.get_center() + UP * 0.45)
        bolt2 = lightning(scale=0.8, color=RED).move_to(a2.get_center() + UP * 0.45)
        e1 = Text("energy", font_size=14, color=AMBER).next_to(bolt1, UP, buff=0.04)
        e2 = Text("energy", font_size=14, color=RED).next_to(bolt2, UP, buff=0.04)
        # the SAME cat image moves; nothing new spawns
        self.play(cat.animate.move_to(cpu.get_center()), FadeIn(bolt1), FadeIn(e1),
                  Flash(a1.get_center(), color=AMBER, line_length=0.2), run_time=0.9)
        self.play(cat.animate.move_to(gpu.get_center()), FadeIn(bolt2), FadeIn(e2),
                  Flash(a2.get_center(), color=RED, line_length=0.2), run_time=0.9)

        capC = caption("Conventional pipelines move the whole image between chips - "
                       "the transfer burns energy and bandwidth.")
        self.play(FadeIn(capC), run_time=0.7)
        self.wait(1.4)
        self.play(FadeOut(VGroup(sensor, sensor_lbl, cat, cpu, gpu, cpu_die, gpu_die,
                                 a1, a2, bolt1, bolt2, e1, e2, capC)), run_time=0.7)
