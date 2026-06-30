"""S2 - High-level die overview. Where the sensor and the 2D fabric live, and
how a frame moves through the chip. Mirrors fyp_diagram/selected/asic_die_v2.drawio.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, RoundedRectangle, DashedVMobject, Arrow, Text,
    FadeIn, FadeOut, Create, GrowArrow, Indicate,
    LEFT, RIGHT, UP, DOWN,
)
from common.assets import cat_image
from common.pixel_grid import PixelGrid
from common.blocks import labeled_box
from common.palette import (
    FG, MUTED, TEAL, AMBER, BLUE, GREEN, PURPLE, ORANGE, SANS, MONO, caption,
)


def dashed(rect):
    return DashedVMobject(rect, num_dashes=80, dashed_ratio=0.55)


class DieOverview(Scene):
    def construct(self):
        # ---- two dies -----------------------------------------------------
        near = RoundedRectangle(width=10.6, height=5.4, corner_radius=0.12,
                                stroke_color=GREEN, stroke_width=2)
        near.move_to(LEFT * 1.7 + DOWN * 0.2)
        soc = RoundedRectangle(width=2.6, height=5.4, corner_radius=0.12,
                               stroke_color=MUTED, stroke_width=2)
        soc.move_to(RIGHT * 5.45 + DOWN * 0.2)
        near_d, soc_d = dashed(near), dashed(soc)
        near_d.set_stroke(GREEN, 2, opacity=0.7)
        soc_d.set_stroke(MUTED, 2, opacity=0.7)

        near_lbl = Text("NEAR-SENSOR PROCESSOR DIE", font=SANS, font_size=18,
                        color=GREEN, weight="BOLD")
        near_lbl.move_to(near.get_top() + DOWN * 0.28)
        soc_lbl = Text("SoC DIE", font=SANS, font_size=16, color=MUTED, weight="BOLD")
        soc_lbl.move_to(soc.get_top() + DOWN * 0.28)

        self.play(Create(near_d), Create(soc_d), FadeIn(near_lbl), FadeIn(soc_lbl),
                  run_time=1.0)

        # ---- blocks inside the near-sensor die ----------------------------
        y0 = DOWN * 0.25
        sensor_grid = PixelGrid(cat_image(14), cell_size=0.13)
        sensor_box = labeled_box("", 2.1, 2.1, GREEN, fill_opacity=0.06)
        sensor = VGroup(sensor_box, sensor_grid.move_to(sensor_box))
        sensor.move_to(LEFT * 5.2 + y0)
        sensor_cap = Text("On-Die Image Sensor", font=SANS, font_size=16, color=GREEN)
        sensor_cap.next_to(sensor, DOWN, buff=0.12)

        fbuf = labeled_box("Frame\nBuffer", 0.95, 2.6, GREEN, font_size=16)
        fbuf.move_to(LEFT * 3.4 + y0)

        dma = labeled_box("DMA", 1.2, 0.7, ORANGE, font_size=16)
        dma.move_to(LEFT * 1.9 + UP * 1.15)

        fabric = labeled_box("2D Vector\nFabric", 2.4, 2.6, PURPLE, font_size=20)
        fabric.move_to(LEFT * 0.4 + y0)
        fabric_cap = Text("2D T1 Vector Processing Fabric", font=SANS,
                          font_size=15, color=PURPLE)
        fabric_cap.next_to(fabric, DOWN, buff=0.12)

        amba = labeled_box("AMBA", 0.85, 3.0, BLUE, font_size=16)
        amba.move_to(RIGHT * 1.4 + y0)

        scalar = labeled_box("Off-Chip\nScalar Core\n(DDR / Host)", 2.2, 3.4, MUTED,
                             font_size=16)
        scalar.move_to(RIGHT * 5.45 + y0)

        self.play(FadeIn(sensor), FadeIn(sensor_cap), run_time=0.7)
        self.play(FadeIn(fbuf), FadeIn(dma), run_time=0.5)
        self.play(FadeIn(fabric), FadeIn(fabric_cap), run_time=0.6)
        self.play(FadeIn(amba), FadeIn(scalar), run_time=0.6)

        # ---- the frame moves on-chip: sensor -> buffer -> DMA -> fabric ----
        def arr(a, b, color, sw=3):
            ar = Arrow(a, b, color=color, buff=0.08, stroke_width=sw,
                       max_tip_length_to_length_ratio=0.12)
            return ar

        a1 = arr(sensor_box.get_right(), fbuf.get_left(), GREEN)
        a2 = arr(fbuf.get_top(), dma.get_left(), GREEN)
        a3 = arr(dma.get_right(), fabric.get_top() + LEFT * 0.4, GREEN)
        flow_lbl = Text("camera frame", font=SANS, font_size=15, color=GREEN)
        flow_lbl.next_to(a1, UP, buff=0.06)
        self.play(GrowArrow(a1), GrowArrow(a2), GrowArrow(a3), FadeIn(flow_lbl),
                  run_time=0.9)

        # a cat token travels along the path into the fabric
        token = PixelGrid(cat_image(12), cell_size=0.06).move_to(sensor_grid)
        self.add(token)
        self.play(token.animate.move_to(fbuf), run_time=0.7)
        self.play(token.animate.move_to(dma), run_time=0.5)
        self.play(token.animate.move_to(fabric).scale(2.2), run_time=0.8)
        self.play(Indicate(fabric.box, color=PURPLE, scale_factor=1.06))

        # ---- across the die boundary: instructions in, results out --------
        in_arr = arr(scalar.get_left(), amba.get_right(), BLUE)
        in2 = arr(amba.get_left(), fabric.get_right(), PURPLE)
        in_lbl = Text("vector instruction", font=SANS, font_size=15, color=BLUE)
        in_lbl.next_to(in_arr, UP, buff=0.05)
        out_arr = Arrow(fabric.get_bottom() + RIGHT * 0.6, scalar.get_bottom() + LEFT * 0.1,
                        color=AMBER, buff=0.08, stroke_width=3,
                        max_tip_length_to_length_ratio=0.05)
        out_lbl = Text("scalar result out", font=SANS, font_size=15, color=AMBER)
        out_lbl.next_to(out_arr, DOWN, buff=0.05)
        self.play(GrowArrow(in_arr), GrowArrow(in2), FadeIn(in_lbl), run_time=0.7)
        self.play(GrowArrow(out_arr), FadeIn(out_lbl), run_time=0.7)

        cap = caption("Sensor and 2D fabric share one die - the frame moves on-chip; "
                      "only instructions in, results out.")
        self.play(FadeIn(cap), run_time=0.8)
        self.wait(1.4)

        # ---- hand off to the next scene: focus the fabric -----------------
        keep = VGroup(fabric, fabric_cap, token)
        everything = VGroup(
            near_d, soc_d, near_lbl, soc_lbl, sensor, sensor_cap, fbuf, dma,
            amba, scalar, a1, a2, a3, flow_lbl, in_arr, in2, in_lbl,
            out_arr, out_lbl, cap,
        )
        self.play(FadeOut(everything), run_time=0.8)
        self.play(Indicate(fabric.box, color=PURPLE, scale_factor=1.1), run_time=0.6)
        self.wait(0.4)
        self.play(FadeOut(keep), run_time=0.6)
