"""Scene 3.10 - The BONSAI SoC dataflow, straight after "How does it work?".
Framing and object movements follow drawio_scene_ref/bonsai_soc.drawio
(7 numbered sub-scene frames). One continuous scene: every object is dragged
to its next location (never respawned) so the viewer tracks the direction of
data travel and the structure of the architecture.

Sub-scenes (drawio frames):
  1. cat + near-sensor die (sensor -> scratchpad -> Bonsai) and a separate
     CPU die; two capture lines from the cat to the sensor.
  2. the cat is captured into the sensor.
  3. the cat moves into the scratchpad memory.
  4. the two dies slide apart; the CPU sends "Instructions" to Bonsai.
  5. the cat moves into the Bonsai processor.
  6. the cat is processed into a compact result (data block); a
     "processed results" arrow appears from Bonsai to the CPU.
  7. the result travels across to the CPU.

Elements reuse the builders from scene 3.5 (same camera sensor, the same
green Scratchpad Memory / purple Bonsai Processor / grey CPU chips, dashed die
outlines, the pixel cat, the coordinate data block) for continuity.

The final diagram is LEFT ON SCREEN: Scene 6 (programmability) re-adds the
exact same end state via `build_end_state()` and rewinds it (matched cut).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Text, Line, Arrow, RoundedRectangle,
    FadeIn, FadeOut, Create, GrowArrow, ReplacementTransform, Flash,
    LEFT, RIGHT, UP, DOWN,
)
from common.layout import make_cat
from common.icons import camera_sensor, chip
from common.palette import (
    BG, FG, MUTED, TEAL, AMBER, BLUE, GREEN, PURPLE, RED, SANS, caption,
)
from scenes.s3_conventional import die_around
from scenes.s3_5_compare import data_block

# --- layout (from the drawio frames; y = 0 row, captions at the bottom) -----
CAT_START = np.array([-5.8, 0.0, 0.0])
SENSOR_X, SCRATCH_X, BONSAI_X, CPU_X = -2.2, 0.0, 2.2, 4.45
# frames 4-7: the near-sensor die slides left, the CPU die slides right
NEAR_SHIFT = LEFT * 1.2
CPU_SHIFT = RIGHT * 0.95
CAT_IN_BLOCK_W = 0.95


def _p(x, y):
    return np.array([x, y, 0.0])


def build_pipeline():
    """The two dies at the frames 1-3 (pre-shift) layout, same elements as
    scene 3.5. Returns the group with named parts so both this scene and
    Scene 6 construct the identical object."""
    sensor = camera_sensor(1.0).move_to(_p(SENSOR_X, 0))
    sensor_lbl = Text("sensor", font=SANS, font_size=13, color=BLUE)
    sensor_lbl.next_to(sensor, UP, buff=0.12)
    scratch = chip("Global\nMemory", w=1.5, h=1.3, color=GREEN,
                   font_size=15).move_to(_p(SCRATCH_X, 0))
    bonsai = chip("BONSAI\nProcessor", w=1.5, h=1.3, color=PURPLE,
                  font_size=15, weight="BOLD").move_to(_p(BONSAI_X, 0))
    a1 = Arrow(sensor.get_right(), scratch.get_left(), buff=0.06, stroke_width=3,
               max_tip_length_to_length_ratio=0.25).set_color_by_gradient(GREEN, TEAL)
    a2 = Arrow(scratch.get_right(), bonsai.get_left(), buff=0.06, stroke_width=3,
               max_tip_length_to_length_ratio=0.25).set_color_by_gradient(GREEN, TEAL)
    near_inner = VGroup(sensor, scratch, bonsai)
    near_die = die_around(near_inner, TEAL, pad_w=0.4, pad_h=0.45)
    near_lbl = Text("NEAR-SENSOR DIE", font_size=16, color=TEAL, weight="BOLD")
    near_lbl.next_to(near_die, UP, buff=0.15)
    near_grp = VGroup(near_die, near_lbl, sensor, sensor_lbl, scratch, bonsai,
                      a1, a2)

    cpu = chip("CPU", w=1.2, h=1.2, color=MUTED).move_to(_p(CPU_X, 0))
    cpu_die = die_around(cpu, MUTED)
    cpu_grp = VGroup(cpu_die, cpu)

    g = VGroup(near_grp, cpu_grp)
    g.sensor, g.scratch, g.bonsai, g.a1, g.a2 = sensor, scratch, bonsai, a1, a2
    g.sensor_lbl = sensor_lbl
    g.near_die, g.near_lbl, g.near_grp = near_die, near_lbl, near_grp
    g.cpu, g.cpu_die, g.cpu_grp = cpu, cpu_die, cpu_grp
    return g


def instr_channel(cpu, bonsai, label="Instructions"):
    """Blue instruction arrow CPU -> Bonsai on the upper channel."""
    a = Arrow(cpu.get_left() + UP * 0.4, bonsai.get_right() + UP * 0.4,
              color=BLUE, buff=0.15, stroke_width=3.5,
              max_tip_length_to_length_ratio=0.1)
    lbl = Text(label, font=SANS, font_size=16, color=BLUE)
    lbl.next_to(a, UP, buff=0.1)
    return a, lbl


def results_channel(bonsai, cpu):
    """Amber "processed results" arrow Bonsai -> CPU on the lower channel."""
    a = Arrow(bonsai.get_right() + DOWN * 0.4, cpu.get_left() + DOWN * 0.4,
              color=AMBER, buff=0.15, stroke_width=3.5,
              max_tip_length_to_length_ratio=0.1)
    lbl = Text("processed results", font=SANS, font_size=16, color=AMBER)
    lbl.next_to(a, DOWN, buff=0.1)
    return a, lbl


def make_result():
    """The processed-result block (dark backing keeps it legible on chips)."""
    res_txt = data_block(AMBER)
    res_bg = RoundedRectangle(width=res_txt.width + 0.2, height=res_txt.height + 0.16,
                              corner_radius=0.06, stroke_width=0).set_fill(BG, 0.88)
    return VGroup(res_bg, res_txt)


def build_end_state():
    """The exact final frame of this scene (shifted dies, results channel,
    result sitting in the CPU; the instruction arrow is gone - its job is
    done) - Scene 6 re-adds this for the matched cut."""
    p = build_pipeline()
    p.near_grp.shift(NEAR_SHIFT)
    p.cpu_grp.shift(CPU_SHIFT)
    res_a, res_l = results_channel(p.bonsai, p.cpu)
    result = make_result().move_to(p.cpu)
    g = VGroup(p, res_a, res_l, result)
    g.pipe = p
    g.res_a, g.res_l = res_a, res_l
    g.result = result
    return g


class SocDataflow(Scene):
    def construct(self):
        p = build_pipeline()
        sensor, scratch, bonsai, cpu = p.sensor, p.scratch, p.bonsai, p.cpu

        # ---- 1: the setup - cat looks into the pipeline ---------------------
        cat = make_cat(n=24).scale_to_fit_width(1.7).move_to(CAT_START)
        cap1 = caption("Sensor, memory and BONSAI share one die - the CPU sits on its own.")
        self.play(FadeIn(p.near_grp), FadeIn(p.cpu_grp), FadeIn(cat), FadeIn(cap1),
                  run_time=0.9)

        l1 = Line(cat.get_corner(UP + RIGHT), sensor.get_left(), stroke_width=2).set_stroke(TEAL, opacity=0.8)
        l2 = Line(cat.get_corner(DOWN + RIGHT), sensor.get_left(), stroke_width=2).set_stroke(TEAL, opacity=0.8)
        self.play(Create(l1), Create(l2), run_time=0.6)
        self.wait(0.6)

        # ---- 2: capture - the cat drags into the sensor ---------------------
        cap2 = caption("The sensor captures the image...")
        self.play(FadeOut(VGroup(l1, l2)), FadeOut(cap1), FadeIn(cap2),
                  cat.animate.scale_to_fit_width(CAT_IN_BLOCK_W).move_to(sensor),
                  run_time=0.9)
        self.wait(0.6)

        # ---- 3: into the global memory ---------------------------------------
        cap3 = caption("...and writes it into the global memory, still on-die.")
        self.play(FadeOut(cap2), FadeIn(cap3),
                  cat.animate.move_to(scratch),
                  Flash(p.a1.get_center(), color=TEAL, line_length=0.12),
                  run_time=0.8)
        self.wait(0.8)

        # ---- 4: dies slide apart; the CPU sends "Load image" ----------------
        cap4 = caption("Instruction: Load image")
        self.play(FadeOut(cap3),
                  VGroup(p.near_grp, cat).animate.shift(NEAR_SHIFT),
                  p.cpu_grp.animate.shift(CPU_SHIFT),
                  run_time=0.9)
        instr, instr_l = instr_channel(cpu, bonsai, "Load image")
        self.play(GrowArrow(instr), FadeIn(instr_l), FadeIn(cap4), run_time=0.8)
        self.wait(0.8)

        # ---- 5: the image loads into the BONSAI Processor -------------------
        cap5 = caption("The whole image loads into the BONSAI Processor...")
        self.play(FadeOut(cap4), FadeIn(cap5),
                  cat.animate.move_to(bonsai),
                  Flash(p.a2.get_center(), color=TEAL, line_length=0.12),
                  run_time=0.8)
        self.wait(0.8)

        # ---- 6: next instruction - "Process image" ---------------------------
        cap6 = caption("Instruction: Process image")
        instr_l2 = Text("Process image", font=SANS, font_size=16, color=BLUE)
        instr_l2.next_to(instr, UP, buff=0.1)
        self.play(FadeOut(cap5), FadeIn(cap6),
                  ReplacementTransform(instr_l, instr_l2),
                  Flash(instr.get_center(), color=BLUE, line_length=0.12),
                  run_time=0.7)
        result = make_result().move_to(bonsai)
        res_a, res_l = results_channel(bonsai, cpu)
        self.play(ReplacementTransform(cat, result), run_time=0.9)
        self.play(GrowArrow(res_a), FadeIn(res_l), run_time=0.7)
        self.wait(0.8)

        # ---- 7: the result crosses to the CPU; the instruction is done ------
        # two-line caption with "small" / "large" aligned above each other
        l1 = VGroup(
            Text("Only the", font=SANS, font_size=24, color=FG),
            Text("small result leaves", font=SANS, font_size=24, color=GREEN,
                 weight="BOLD"),
            Text("the die,", font=SANS, font_size=24, color=FG),
        ).arrange(RIGHT, buff=0.15).move_to(_p(0, -2.95))
        l2 = VGroup(
            Text("the", font=SANS, font_size=24, color=FG),
            Text("large image never leaves", font=SANS, font_size=24, color=RED,
                 weight="BOLD"),
            Text("die.", font=SANS, font_size=24, color=FG),
        ).arrange(RIGHT, buff=0.15).move_to(_p(0, -3.5))
        l2.shift(RIGHT * (l1[1].get_left()[0] - l2[1].get_left()[0]))
        cap7 = VGroup(l1, l2)
        self.play(FadeOut(cap6), FadeIn(cap7),
                  FadeOut(instr), FadeOut(instr_l2),
                  result.animate.move_to(cpu), run_time=0.9)
        # the frame is done - end of the pipeline
        tick = Text("✓", font=SANS, font_size=30, color=GREEN, weight="BOLD")
        tick.move_to(cpu.get_corner(UP + RIGHT) + np.array([0.24, 0.24, 0.0]))
        end_l = Text("frame done - end of pipeline", font=SANS, font_size=15,
                     color=GREEN).next_to(p.cpu_die, UP, buff=0.15)
        self.play(FadeIn(tick, scale=1.4), FadeIn(end_l), run_time=0.5)
        self.wait(1.6)

        # keep the diagram for the matched cut into Scene 6 (rewind); the
        # trip markers leave with the caption
        self.play(FadeOut(cap7), FadeOut(tick), FadeOut(end_l), run_time=0.6)
        self.wait(0.2)
