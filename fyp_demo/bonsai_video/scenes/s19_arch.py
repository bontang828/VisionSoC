"""Scene 19 - Zoom back out to the BONSAI architecture again."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import Scene, FadeIn, FadeOut, DOWN
from common.arch import bonsai_floorplan
from common.palette import caption


class ArchRecap(Scene):
    def construct(self):
        fp = bonsai_floorplan(scale=1.0).move_to(DOWN * 0.25)
        self.play(FadeIn(fp), run_time=0.9)
        cap = caption("Register file, compute lanes, LSU, decoder - all on the BONSAI core.")
        self.play(FadeIn(cap), run_time=0.6)
        self.wait(1.4)
        self.play(FadeOut(fp), FadeOut(cap), run_time=0.8)
