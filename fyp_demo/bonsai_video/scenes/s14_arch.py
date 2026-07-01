"""Scene 14 - Zoom back out to the full BONSAI architecture."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import Scene, FadeIn, FadeOut, DOWN
from common.arch import bonsai_floorplan
from common.palette import caption


class ArchOverview(Scene):
    def construct(self):
        fp = bonsai_floorplan(scale=1.0).move_to(DOWN * 0.25)
        self.play(FadeIn(fp), run_time=1.0)
        cap = caption("The BONSAI core: register file, compute lanes, LSU, and decoder.")
        self.play(FadeIn(cap), run_time=0.7)
        self.wait(1.6)
        self.play(FadeOut(fp), FadeOut(cap), run_time=0.8)
