"""S3 - Inside the 2D fabric: one vle8.v turns the whole frame into a single
square vector register. Pixel (r,c) -> register cell (r,c)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Square, RoundedRectangle, Arrow, Text,
    FadeIn, FadeOut, Write, Create, Indicate, GrowArrow, LaggedStart,
    LEFT, RIGHT, UP, DOWN,
)
from common.assets import cat_image
from common.pixel_grid import PixelGrid
from common.palette import (
    FG, MUTED, TEAL, AMBER, BLUE, GREEN, PURPLE, MONO, SANS, caption,
)

N = 20
CELL = 0.205
GRID_Y = -0.55
X_OFF = 3.6


class LoadIntoRegister(Scene):
    def construct(self):
        img = cat_image(N)

        context = Text("inside the 2D vector fabric", font=SANS, font_size=20,
                       color=PURPLE).move_to(UP * 3.55)
        code = Text("vle8.v v8, (frame)", font=MONO, font_size=34, color=FG,
                    t2c={"vle8.v": BLUE, "v8": TEAL, "(frame)": AMBER}).move_to(UP * 2.9)

        die = RoundedRectangle(width=12.8, height=5.7, corner_radius=0.15,
                               stroke_color=PURPLE, stroke_width=2.0)
        die.set_stroke(opacity=0.5).set_fill(opacity=0.0).move_to(DOWN * 0.5)

        self.play(FadeIn(context), Write(code), run_time=0.9)
        self.play(Create(die), run_time=0.7)

        source = PixelGrid(img, cell_size=CELL).move_to(LEFT * X_OFF + UP * GRID_Y)
        src_label = Text("incoming frame", font=SANS, font_size=20, color=MUTED)
        src_label.next_to(source, UP, buff=0.16)
        px_label = Text("128 x 128  =  16,384 px", font=MONO, font_size=18, color=FG)
        px_label.next_to(source, DOWN, buff=0.16)
        self.play(LaggedStart(*[FadeIn(c) for c in source.cell_list],
                              lag_ratio=0.003, run_time=1.2),
                  FadeIn(src_label), FadeIn(px_label))

        reg_center = RIGHT * X_OFF + UP * GRID_Y
        reg_frame = Square(side_length=CELL * N).set_stroke(TEAL, 2.5)
        reg_frame.set_fill(opacity=0.0).move_to(reg_center)
        reg_label = Text("v8   ·   2D vector register", font=MONO, font_size=20, color=TEAL)
        reg_label.next_to(reg_frame, UP, buff=0.16)
        self.play(Create(reg_frame), FadeIn(reg_label), run_time=0.6)

        flow = Arrow(source.get_right() + RIGHT * 0.1, reg_frame.get_left() + LEFT * 0.1,
                     color=GREEN, stroke_width=4, buff=0.05,
                     max_tip_length_to_length_ratio=0.06)
        flow_lbl = Text("DMA", font=MONO, font_size=18, color=GREEN).next_to(flow, UP, buff=0.1)
        self.play(GrowArrow(flow), FadeIn(flow_lbl))

        flying = PixelGrid(img, cell_size=CELL).move_to(source.get_center())
        self.add(flying)
        self.play(flying.animate.move_to(reg_center), run_time=1.4)

        r, c = 5, 7
        src_hi = Square(CELL).move_to(source.cell_center(r, c)).set_stroke(AMBER, 4).set_fill(opacity=0)
        dst_hi = Square(CELL).move_to(flying.cell_center(r, c)).set_stroke(AMBER, 4).set_fill(opacity=0)
        map_lbl = Text("pixel (r, c)  ->  cell (r, c)", font=MONO, font_size=20,
                       color=AMBER).move_to(reg_center + DOWN * 2.25)
        self.play(Create(src_hi), Create(dst_hi))
        self.play(Indicate(src_hi, color=AMBER), Indicate(dst_hi, color=AMBER), FadeIn(map_lbl))
        self.play(FadeOut(VGroup(src_hi, dst_hi, map_lbl)), run_time=0.5)

        counter = VGroup(
            Text("1 instruction", font=SANS, font_size=24, color=TEAL),
            Text("16,384 px", font=SANS, font_size=22, color=GREEN),
        ).arrange(DOWN, buff=0.12).move_to(flow.get_center() + DOWN * 1.4)
        cap = caption("The image's 2D shape is preserved - it becomes one square register.")
        self.play(FadeIn(counter), FadeIn(cap), run_time=0.9)
        self.wait(1.4)
        self.play(FadeOut(VGroup(context, code, die, source, src_label, px_label,
                                 reg_frame, reg_label, flow, flow_lbl, flying,
                                 counter, cap)), run_time=0.7)
