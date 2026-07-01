"""Scene 17 - The instruction blob expands into a list of RVV instructions."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Text, FadeIn, FadeOut, LaggedStart, ReplacementTransform,
    UP, DOWN, LEFT,
)
from common.icons import chip
from common.palette import FG, MUTED, TEAL, BLUE, AMBER, SANS, MONO, caption

INSTRS = [
    "vadd.vv     v1, v2, v3",
    "vsub.vv     v4, v5, v6",
    "vsll.vi     v7, v8, 2",
    "vrgather.vv v9, v10, v11",
    "...",
]


class InstrList(Scene):
    def construct(self):
        blob = chip("instruction", w=2.6, h=1.0, color=BLUE, font_size=24).move_to(UP * 0.3)
        self.play(FadeIn(blob), run_time=0.6)

        def mk(s):
            if s == "...":
                return Text(s, font=MONO, font_size=28, color=MUTED)
            return Text(s, font=MONO, font_size=28, color=FG, t2c={s.split()[0]: TEAL})

        lines = VGroup(*[mk(s) for s in INSTRS])
        lines.arrange(DOWN, buff=0.28, aligned_edge=LEFT).move_to(UP * 0.2)

        self.play(ReplacementTransform(blob, lines[0]),
                  LaggedStart(*[FadeIn(ln, shift=DOWN * 0.1) for ln in lines[1:]],
                              lag_ratio=0.25, run_time=1.4))
        cap = caption("Standard RISC-V Vector instructions.")
        self.play(FadeIn(cap), run_time=0.6)
        self.wait(1.6)
        self.play(FadeOut(VGroup(lines, cap)), run_time=0.7)
