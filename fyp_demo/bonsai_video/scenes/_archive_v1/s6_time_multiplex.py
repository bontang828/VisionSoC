"""S6 - How it really runs. The 128x128 square is logical; it is time-multiplexed
over just 2 physical lanes, replayed x128. Small, scalable silicon."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Square, Rectangle, Text, Integer, ValueTracker, Arrow,
    FadeIn, FadeOut, Create, GrowArrow, LaggedStart,
    LEFT, RIGHT, UP, DOWN,
)
from common.assets import cat_image
from common.pixel_grid import PixelGrid
from common.palette import FG, MUTED, TEAL, AMBER, BLUE, PURPLE, MONO, SANS, caption

N = 16
CELL = 0.26


class TimeMultiplex(Scene):
    def construct(self):
        img = cat_image(N)
        grid = PixelGrid(img, cell_size=CELL).move_to(RIGHT * 2.6 + DOWN * 0.2)
        for c in grid.cell_list:
            c.set_opacity(0.0)
        frame = Square(CELL * N).set_stroke(TEAL, 2).set_fill(opacity=0).move_to(grid)
        glabel = Text("logical 128 x 128 register", font=SANS, font_size=20,
                      color=TEAL).next_to(frame, UP, buff=0.16)
        self.play(Create(frame), FadeIn(glabel), run_time=0.8)

        # two physical lanes on the left
        lane0 = Rectangle(width=1.8, height=0.7).set_stroke(PURPLE, 2).set_fill(PURPLE, 0.15)
        lane1 = lane0.copy()
        lanes = VGroup(lane0, lane1).arrange(DOWN, buff=0.4).move_to(LEFT * 4.0 + DOWN * 0.2)
        l0t = Text("Lane 0", font=MONO, font_size=18, color=FG).move_to(lane0)
        l1t = Text("Lane 1", font=MONO, font_size=18, color=FG).move_to(lane1)
        lanes_lbl = Text("2 physical lanes", font=SANS, font_size=20, color=PURPLE)
        lanes_lbl.next_to(lanes, UP, buff=0.25)
        self.play(FadeIn(lanes), FadeIn(l0t), FadeIn(l1t), FadeIn(lanes_lbl), run_time=0.7)

        feed = Arrow(lanes.get_right(), frame.get_left(), color=PURPLE, buff=0.2,
                     stroke_width=4, max_tip_length_to_length_ratio=0.06)
        self.play(GrowArrow(feed), run_time=0.5)

        # replay counter
        vt = ValueTracker(0)
        cnt = Integer(0, color=AMBER, font_size=30)
        cnt.add_updater(lambda m: m.set_value(int(vt.get_value())))
        cnt_lbl = Text("hw-row  (of 128):", font=SANS, font_size=22, color=MUTED)
        counter = VGroup(cnt_lbl, cnt).arrange(RIGHT, buff=0.18)
        counter.move_to(LEFT * 4.0 + UP * 2.4)
        self.play(FadeIn(counter), run_time=0.4)

        # paint the logical grid row by row as lanes replay
        row_anims = []
        for r in range(N):
            row_cells = [grid.cell(r, c) for c in range(N)]
            row_anims.append(VGroup(*row_cells).animate.set_opacity(1.0))
        self.play(
            LaggedStart(*row_anims, lag_ratio=0.5),
            vt.animate.set_value(127),
            run_time=3.0,
        )
        cnt.clear_updaters()
        cnt.set_value(127)

        replay = Text("replayed x128", font=MONO, font_size=22, color=PURPLE)
        replay.next_to(lanes, DOWN, buff=0.3)
        cap = caption("The square is logical - 2 lanes, replayed x128. Small, scalable silicon.")
        self.play(FadeIn(replay), FadeIn(cap), run_time=0.8)
        self.wait(1.4)
        self.play(FadeOut(VGroup(grid, frame, glabel, lanes, l0t, l1t, lanes_lbl,
                                 feed, counter, replay, cap)), run_time=0.7)
