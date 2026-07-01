"""Scene 3.7 - Contribution 2: live camera FPGA prototype.
AMD Kria KV260 board photo on the right, bullets reveal on the left. Part of the
"Our Contributions" interlude (3.6 - 3.8).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, Group, VGroup, Text, Dot, ImageMobject, RoundedRectangle,
    FadeIn, FadeOut, UP, DOWN, LEFT, RIGHT,
)
from common.palette import FG, MUTED, TEAL, AMBER, SANS

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KV260 = os.path.join(_ROOT, "kv260_image_background_removed.png")


def _p(x, y):
    return np.array([x, y, 0.0])


def kicker(step):
    return Text(f"CONTRIBUTION  {step} / 3", font=SANS, font_size=18,
                color=TEAL).to_corner(UP + LEFT, buff=0.55)


def bullet(text, color=TEAL, fs=24):
    d = Dot(radius=0.07, color=color)
    t = Text(text, font=SANS, font_size=fs, color=FG).next_to(d, RIGHT, buff=0.22)
    return VGroup(d, t)


class ContributionFPGA(Scene):
    def construct(self):
        tag = kicker(2)
        title = Text("Live camera FPGA prototype", font=SANS, font_size=34,
                     color=FG).to_edge(UP, buff=1.0)

        # --- KV260 board photo (right) ---
        board = ImageMobject(KV260)
        board.scale_to_fit_width(5.4).move_to(_p(3.4, -0.35))
        cap = Text("AMD Kria KV260", font=SANS, font_size=20, color=MUTED)
        cap.next_to(board, DOWN, buff=0.2)
        board_grp = Group(board, cap)

        # --- bullets (left) ---
        bullets = VGroup(
            bullet("Programmable kernel"),
            bullet("Real-time capture and process"),
            bullet("Time-multiplexed execution"),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.7).move_to(_p(-3.7, -0.35))

        self.play(FadeIn(tag), run_time=0.4)
        self.play(FadeIn(title, shift=UP * 0.15), run_time=0.6)
        self.play(FadeIn(board_grp, shift=LEFT * 0.3), run_time=0.8)
        for b in bullets:
            self.play(FadeIn(b, shift=RIGHT * 0.2), run_time=0.5)
        self.wait(1.6)

        self.play(FadeOut(Group(tag, title, board_grp, bullets)), run_time=0.8)
