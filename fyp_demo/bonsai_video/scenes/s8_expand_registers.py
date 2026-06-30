"""Scene 8 - Zoom into the register file. The SAME simplified stack (32 plain
planes) moves to centre and its planes distribute into 32 individual square
registers, named v0..v31 in place. No second/complex model is used."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Text, FadeIn, FadeOut, LaggedStart, ORIGIN, UP, DOWN,
)
from common.arch import make_reg_file, register_array
from common.layout import RF_POS, RF_SIZE
from common.palette import FG, TEAL, BLUE, MONO, SANS, caption

ARRAY_SHIFT = UP * 0.35
ARRAY_SIZE = 0.95


def distributed_targets():
    arr = register_array(rows=4, cols=8, size=ARRAY_SIZE, grid=False, color=BLUE).shift(ARRAY_SHIFT)
    return [p.get_center() for p in arr.planes]


class ExpandRegisters(Scene):
    def construct(self):
        stack = make_reg_file(size=RF_SIZE).move_to(RF_POS)   # carried in from Scene 5
        self.add(stack)
        cap0 = caption("Zoom into the register file.")
        self.play(FadeIn(cap0), run_time=0.5)
        self.wait(0.6)

        self.play(stack.animate.move_to(ORIGIN + ARRAY_SHIFT), FadeOut(cap0), run_time=0.7)
        targets = distributed_targets()
        scale_f = ARRAY_SIZE / RF_SIZE
        planes = stack.planes
        self.play(
            LaggedStart(*[p.animate.move_to(t).scale(scale_f)
                          .set_fill(BLUE, 0.10).set_stroke(BLUE, width=2.5, opacity=1.0)
                          for p, t in zip(planes, targets)], lag_ratio=0.04),
            run_time=2.2,
        )

        labels = VGroup()
        for i, p in enumerate(planes):
            labels.add(Text(f"v{i}", font=MONO, font_size=18, color=TEAL).next_to(p, DOWN, buff=0.08))
        cap = caption("The register file is 32 two-dimensional square registers: v0 to v31.")
        self.play(LaggedStart(*[FadeIn(t) for t in labels], lag_ratio=0.03, run_time=1.6),
                  FadeIn(cap))
        self.wait(1.6)
        self.play(FadeOut(cap), run_time=0.5)
        self.wait(0.2)
