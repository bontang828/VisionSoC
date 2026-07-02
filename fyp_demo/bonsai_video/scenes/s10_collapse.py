"""Scene 10 - Collapse the 32 planes back into the SAME simplified stacked
register file (the same planes move back; no different model). Leaves a stack at
centre for Scene 11."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Text, FadeIn, FadeOut, LaggedStart, ORIGIN, UP, DOWN,
)
from common.arch import register_array, rf_plane_opacity
from common.palette import FG, TEAL, BLUE, MONO, SANS, caption

ARRAY_SHIFT = UP * 0.35
ARRAY_SIZE = 0.95
RF_SIZE = 2.0
DX, DY = 0.022, 0.018          # match make_reg_file offsets


class CollapseRegisters(Scene):
    def construct(self):
        # carry-in: the 32 named planes (matching the end of Scene 8)
        arr = register_array(rows=4, cols=8, size=ARRAY_SIZE, grid=False, color=BLUE).shift(ARRAY_SHIFT)
        planes = arr.planes
        labels = VGroup()
        for i, p in enumerate(planes):
            labels.add(Text(f"v{i}", font=MONO, font_size=18, color=TEAL).next_to(p, DOWN, buff=0.08))
        self.add(arr, labels)
        self.wait(0.4)

        # collapse: planes fly back into the simplified (centred) stack
        self.play(FadeOut(labels), run_time=0.4)
        scale_up = RF_SIZE / ARRAY_SIZE
        n = len(planes)

        def collapse_anim(i, p):
            so, fo = rf_plane_opacity(i, n)
            tgt = ORIGIN + np.array([DX * (i - (n - 1) / 2), DY * (i - (n - 1) / 2), 0.0])
            return (p.animate.move_to(tgt).scale(scale_up)
                    .set_stroke(BLUE, opacity=so).set_fill(BLUE, opacity=fo))

        self.play(
            LaggedStart(*[collapse_anim(i, p) for i, p in enumerate(planes)], lag_ratio=0.03),
            run_time=1.8,
        )
        self.wait(1.0)
