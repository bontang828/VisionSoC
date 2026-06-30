"""Scene 9 - Name each square register plane: v0 .. v31."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Text, FadeIn, FadeOut, LaggedStart, DOWN, UP,
)
from common.arch import register_array
from common.palette import FG, TEAL, BLUE, MONO, SANS, caption


class NameRegisters(Scene):
    def construct(self):
        array = register_array(rows=4, cols=8, size=0.92, color=BLUE)
        array.shift(UP * 0.45)
        self.play(LaggedStart(*[FadeIn(p) for p in array.planes],
                              lag_ratio=0.02, run_time=1.0))

        labels = VGroup()
        for i, p in enumerate(array.planes):
            t = Text(f"v{i}", font=MONO, font_size=18, color=TEAL)
            t.next_to(p, DOWN, buff=0.08)
            labels.add(t)

        cap = caption("32 architectural vector registers:  v0 to v31.")
        self.play(LaggedStart(*[FadeIn(t) for t in labels], lag_ratio=0.03, run_time=1.8),
                  FadeIn(cap))
        self.wait(1.8)
        self.play(FadeOut(VGroup(array, labels, cap)), run_time=0.8)
