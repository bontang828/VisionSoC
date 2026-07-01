"""Scene 18 - Each instruction compiles to machine code; open-source toolchain."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Text, Arrow, FadeIn, FadeOut, GrowArrow, LaggedStart,
    LEFT, RIGHT, UP, DOWN,
)
from common.palette import FG, MUTED, TEAL, AMBER, BLUE, SANS, MONO, caption

ROWS = [
    ("vadd.vv     v1, v2, v3", "0x0220C0D7"),
    ("vsub.vv     v4, v5, v6", "0x0A53A1D7"),
    ("vsll.vi     v7, v8, 2", "0x96841457"),
    ("vrgather.vv v9, v10, v11", "0x32A584D7"),
]


class Compile(Scene):
    def construct(self):
        asm = VGroup(*[Text(s, font=MONO, font_size=26, color=FG, t2c={s.split()[0]: TEAL})
                       for s, _ in ROWS])
        asm.arrange(DOWN, buff=0.45, aligned_edge=LEFT).move_to(LEFT * 3.4)
        code = VGroup(*[Text(h, font=MONO, font_size=26, color=AMBER) for _, h in ROWS])
        for ln, cd in zip(asm, code):
            cd.move_to([3.6, ln.get_center()[1], 0])
        arrows = VGroup(*[Arrow(ln.get_right(), cd.get_left(), color=MUTED, buff=0.3,
                               stroke_width=3, max_tip_length_to_length_ratio=0.12)
                          for ln, cd in zip(asm, code)])

        self.play(FadeIn(asm), run_time=0.7)
        self.play(LaggedStart(*[GrowArrow(a) for a in arrows], lag_ratio=0.15, run_time=1.0),
                  LaggedStart(*[FadeIn(c, shift=RIGHT * 0.2) for c in code], lag_ratio=0.15, run_time=1.0))

        hdr = Text("instructions", font=SANS, font_size=20, color=MUTED).next_to(asm, UP, buff=0.4)
        hdr2 = Text("machine code", font=SANS, font_size=20, color=MUTED).next_to(code, UP, buff=0.4)
        cap = caption("Compatible with open-source compilers - LLVM & GNU.")
        self.play(FadeIn(hdr), FadeIn(hdr2), FadeIn(cap), run_time=0.7)
        self.wait(1.6)
        self.play(FadeOut(VGroup(asm, code, arrows, hdr, hdr2, cap)), run_time=0.7)
