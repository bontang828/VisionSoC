"""Scene 5 - Into BONSAI.
Carries in the buffer (with the cat) and the BONSAI block from Scene 4 (the
sensor already faded with the die frame). BONSAI shifts to centre and enlarges;
the buffer is dragged to the RIGHT beside the LSU. The image is read row by row,
top to bottom: each row travels buffer -> LSU -> compute lanes, then expands into
the register file, assembling a high-resolution cat. Leaves the register-file
stack for Scene 8."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Square, Text, Arrow, VMobject, FadeIn, FadeOut, GrowArrow,
    Indicate, MoveAlongPath, Succession, LaggedStart, rgb_to_color,
    LEFT, RIGHT, UP, DOWN,
)
from common.layout import make_buffer_cat, RF_POS, FLOORPLAN_CENTER
from common.assets import cat_image
from common.arch import bonsai_floorplan
from common.palette import FG, MUTED, TEAL, GREEN, PURPLE, SANS, caption
from scenes.s4_neurosensor import build_near_sensor_blocks

ROWS = 24        # register-file assembly resolution (higher than before)


class ZoomIntoBonsai(Scene):
    def construct(self):
        # ---- carry-in from Scene 4 (no sensor; it faded with the die) ----
        _sensor, buffer, bonsai, _a_sb, a_bb = build_near_sensor_blocks()
        cat = make_buffer_cat()
        self.add(buffer, bonsai, a_bb, cat)

        fp = bonsai_floorplan(scale=1.0)
        fp.shift(RF_POS - fp.rf.get_center())
        internals = VGroup(fp.rf, fp.rf_lbl, fp.lanes_box, fp.lanes, fp.lanes_lbl,
                           fp.dec, fp.dec_lbl, fp.lsu, fp.lsu_lbl, fp.arrows)

        buf_right = np.array([5.5, fp.lsu.get_center()[1], 0.0])
        self.play(
            FadeOut(a_bb),
            bonsai.animate.move_to(FLOORPLAN_CENTER).scale(4.6),
            buffer.animate.move_to(buf_right),
            cat.animate.move_to(buf_right),
            run_time=1.2,
        )
        self.play(FadeIn(internals), FadeOut(bonsai), run_time=0.9)

        feed = Arrow(buffer.get_left(), fp.lsu.get_right(), color=GREEN, buff=0.12,
                     stroke_width=3, max_tip_length_to_length_ratio=0.16)
        self.play(GrowArrow(feed), run_time=0.5)

        # ---- row-by-row read, top to bottom ------------------------------
        catarr = cat_image(ROWS)
        cell = 2.0 / ROWS
        rf_c = fp.rf.top.get_center()
        lsu_c = fp.lsu.get_center()
        lanes_c = fp.lanes_box.get_center()

        cat_x = cat.get_center()[0]
        cat_top = cat.get_top()[1]
        buf_step = cat.height / ROWS

        def make_row(r):
            row = VGroup()
            for c in range(ROWS):
                sq = Square(cell, stroke_width=0).set_fill(rgb_to_color(catarr[r, c]), 1.0)
                row.add(sq)
            row.arrange(RIGHT, buff=0)                  # full register width (2.0)
            # start at this row's vertical position in the buffer (top -> bottom)
            row.move_to([cat_x, cat_top - (r + 0.5) * buf_step, 0.0])
            row.set_opacity(0.0)
            return row

        def target(r):
            return np.array([rf_c[0], rf_c[1] + ((ROWS - 1) / 2 - r) * cell, 0.0])

        rows = []
        anims = []
        for r in range(ROWS):
            row = make_row(r)
            rows.append(row)
            path = VMobject().set_points_as_corners(
                [row.get_center(), lsu_c, lanes_c, target(r)])
            anims.append(Succession(
                row.animate(run_time=0.12).set_opacity(1.0),
                MoveAlongPath(row, path, run_time=1.2),
            ))
        self.add(*rows)

        cap = caption("Read top to bottom: each row goes buffer -> LSU -> compute lanes "
                      "-> register file.")
        self.play(FadeIn(cap), run_time=0.5)
        self.play(LaggedStart(*anims, lag_ratio=0.07), run_time=5.5)
        self.play(Indicate(fp.rf, color=TEAL, scale_factor=1.05), run_time=0.5)
        self.wait(1.0)

        # ---- carry out: keep the register-file stack for Scene 8 ---------
        rest = VGroup(fp.rf_lbl, fp.lanes_box, fp.lanes, fp.lanes_lbl, fp.dec,
                      fp.dec_lbl, fp.lsu, fp.lsu_lbl, fp.arrows, buffer, cat, feed, cap,
                      *rows)
        self.play(FadeOut(rest), run_time=0.7)
        self.play(fp.rf.animate.move_to(RF_POS), run_time=0.3)
        self.wait(0.2)
