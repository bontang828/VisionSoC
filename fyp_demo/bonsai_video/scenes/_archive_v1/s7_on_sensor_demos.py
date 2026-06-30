"""S7 - Why it matters (data stays on-die, stock RVV) + live-proof montage.
The three demo panels are placeholders to swap with real FPGA captures later."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from colorsys import hsv_to_rgb
from manim import (
    Scene, VGroup, Square, Rectangle, RoundedRectangle, Text, Arrow, Dot,
    FadeIn, FadeOut, Create, GrowArrow, rgb_to_color,
    LEFT, RIGHT, UP, DOWN,
)
from common.assets import cat_image
from common.pixel_grid import PixelGrid
from common.palette import (
    FG, MUTED, TEAL, AMBER, BLUE, GREEN, PURPLE, MONO, SANS, caption,
)


class OnSensorAndDemos(Scene):
    def construct(self):
        # ---- Part A: data stays on-die ------------------------------------
        chip = RoundedRectangle(width=5.2, height=4.2, corner_radius=0.15,
                                stroke_color=GREEN, stroke_width=2.5)
        chip.set_fill(GREEN, 0.04).move_to(LEFT * 2.4)
        chip_lbl = Text("near-sensor die", font=SANS, font_size=18, color=GREEN)
        chip_lbl.move_to(chip.get_top() + DOWN * 0.25)
        cat = PixelGrid(cat_image(16), cell_size=0.16).move_to(chip)
        vrf = Text("image stays resident in the VRF", font=SANS, font_size=18,
                   color=FG).next_to(cat, DOWN, buff=0.2)
        self.play(Create(chip), FadeIn(chip_lbl), FadeIn(cat), FadeIn(vrf), run_time=1.0)

        offchip = Rectangle(width=2.0, height=1.4).set_stroke(MUTED, 2)
        offchip.move_to(RIGHT * 4.2)
        off_lbl = Text("off-chip", font=SANS, font_size=18, color=MUTED).move_to(offchip)
        token = Dot(color=AMBER, radius=0.12).move_to(cat.get_right())
        result_arrow = Arrow(chip.get_right(), offchip.get_left(), color=AMBER,
                             buff=0.15, stroke_width=3)
        only_lbl = Text("only the result leaves", font=SANS, font_size=18,
                        color=AMBER).next_to(result_arrow, UP, buff=0.1)
        self.play(FadeIn(offchip), FadeIn(off_lbl), run_time=0.5)
        self.play(GrowArrow(result_arrow), FadeIn(only_lbl),
                  token.animate.move_to(offchip.get_center()), run_time=1.0)

        capA = caption("Data stays on-die - privacy and bandwidth by construction.  "
                       "Stock RISC-V RVV: plain LLVM / GCC.")
        self.play(FadeIn(capA), run_time=0.8)
        self.wait(1.4)
        self.play(FadeOut(VGroup(chip, chip_lbl, cat, vrf, offchip, off_lbl,
                                 token, result_arrow, only_lbl, capA)), run_time=0.7)

        # ---- Part B: live-proof montage (placeholders) --------------------
        header = Text("Proven on real silicon  -  Live @ 30 FPS  ·  AMD Kria KV260",
                      font=SANS, font_size=26, color=TEAL).move_to(UP * 3.1)
        self.play(FadeIn(header), run_time=0.6)

        p1 = self._panel_camera()
        p2 = self._panel_flow()
        p3 = self._panel_attention()
        panels = VGroup(p1, p2, p3).arrange(RIGHT, buff=0.7).move_to(DOWN * 0.3)

        self.play(FadeIn(p1), run_time=0.6)
        self.play(FadeIn(p2), run_time=0.6)
        self.play(FadeIn(p3), run_time=0.6)

        note = caption("[ placeholders - swap in real camera / optical-flow / matmul captures ]",
                       color=MUTED, size=22)
        self.play(FadeIn(note), run_time=0.6)
        self.wait(1.6)
        self.play(FadeOut(VGroup(header, panels, note)), run_time=0.7)

    # ----- placeholder panel builders --------------------------------------
    def _frame(self, title, color):
        box = RoundedRectangle(width=3.6, height=3.4, corner_radius=0.1,
                               stroke_color=color, stroke_width=2).set_fill(color, 0.04)
        t = Text(title, font=SANS, font_size=20, color=color)
        t.next_to(box.get_top(), DOWN, buff=0.15)
        g = VGroup(box, t)
        g.box = box
        return g

    def _panel_camera(self):
        g = self._frame("camera -> display", GREEN)
        cat = PixelGrid(cat_image(16), cell_size=0.14).move_to(g.box).shift(DOWN * 0.1)
        fps = Text("30 FPS", font=MONO, font_size=20, color=GREEN).next_to(cat, DOWN, buff=0.12)
        g.add(cat, fps)
        return g

    def _panel_flow(self):
        g = self._frame("dense optical flow", AMBER)
        field = VGroup()
        n = 6
        for r in range(n):
            for c in range(n):
                ang = (np.sin(r * 0.9) + np.cos(c * 0.9)) * np.pi
                col = rgb_to_color(hsv_to_rgb((ang / (2 * np.pi)) % 1.0, 0.85, 1.0))
                a = Arrow(ORIGIN_(), RIGHT_() * 0.22, color=col, buff=0,
                          stroke_width=3, max_tip_length_to_length_ratio=0.5)
                a.rotate(ang)
                a.move_to(g.box.get_center() + np.array([(c - n / 2 + 0.5) * 0.4,
                                                         (n / 2 - r - 0.5) * 0.4 - 0.1, 0]))
                field.add(a)
        g.add(field)
        return g

    def _panel_attention(self):
        g = self._frame("matmul -> attention", PURPLE)
        A = PixelGrid(np.random.RandomState(1).rand(8, 8), cell_size=0.12)
        B = PixelGrid(np.random.RandomState(2).rand(8, 8), cell_size=0.12)
        eq = Text("A x Bᵀ = C", font=MONO, font_size=20, color=PURPLE)
        C = PixelGrid(np.random.RandomState(3).rand(8, 8), cell_size=0.12)
        row = VGroup(A, B).arrange(RIGHT, buff=0.25)
        stack = VGroup(row, eq, C).arrange(DOWN, buff=0.15).move_to(g.box).shift(DOWN * 0.1)
        g.add(stack)
        return g


def ORIGIN_():
    return np.array([0.0, 0.0, 0.0])


def RIGHT_():
    return np.array([1.0, 0.0, 0.0])
