"""Scene 7 - Row / column processing, following
drawio_scene_ref/row_col_processing.drawio (11 numbered sub-scene frames).
Continues seamlessly from Scene 6 via a matched cut (re-adds its exact end
state through `build_arch_state`). One instruction - vshift, "slide the
pixels" - is shown to give a DIFFERENT result along rows vs columns:

  1.     vshift arrives at the decoder; instruction tag pinned top-left.
  2.     zoom into the Memory Register File (cat inside) + compute lane.
  3.     "vertical mode: DISABLED" config box appears; the row/column
         processing badge (Contribution 1 callback) pins top-right; a row of
         the cat is highlighted.
  4-6.   the row loads into the compute lane chunk by chunk, is processed
         (pixels slide), and is stored back; the sweep repeats row by row
         until the whole image has slid sideways.
  7-10.  split screen: the setup duplicates to the right with
         "vertical mode: ENABLED" - a COLUMN is highlighted, loads into the
         horizontal lane (transposed look), is processed and stored back;
         the sweep repeats until the image has slid downward.
  11.    side-by-side finish: same instruction, different outcome.

Fades to black at the end - Scene 7.5 (heterogeneous processing) follows and
now owns the register-file handoff to Scene 8.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Text, Arrow, DashedLine, RoundedRectangle,
    SurroundingRectangle, FadeIn, FadeOut, Create, GrowArrow, Indicate,
    ReplacementTransform, TransformFromCopy, rgb_to_color,
    LEFT, RIGHT, UP, DOWN,
)
from common.assets import cat_image
from common.pixel_grid import PixelGrid
from common.palette import (
    BG, FG, MUTED, TEAL, AMBER, BLUE, GREEN, MAGENTA, SANS, MONO, caption,
)
from scenes.s6_programmability import build_arch_state, token
from scenes.s3_6_contribution import icon_rowcol

# --- zoomed memory+compute layout (drawio frames 2-11) -----------------------
N = 24                                  # cat resolution (as loaded in Scene 6)
SHIFT = 6                               # vshift amount (pixels)
ROW_IDX, COL_IDX = 6, 15                # demo row / column
RF_W = 3.6                              # register plane size when zoomed
RF_BIG = np.array([0.0, 0.9, 0.0])      # low enough that the stack depth
LANE_BIG = np.array([0.0, -2.5, 0.0])   # leaves room for the label on top
LANE_W = 5.6
LANE_STRIP_W = 5.1                      # data strip width inside the lane
SPLIT_DX = 3.2                          # panel offset in the split view
MODE_L = np.array([-6.3, -0.8, 0.0])    # mode boxes in the split view
MODE_R = np.array([6.3, -0.8, 0.0])
MODE_FULL = np.array([-5.4, -2.2, 0.0])


def _p(x, y):
    return np.array([x, y, 0.0])


def mode_box(enabled):
    """The row/col access config: 'vertical mode' CSR, DISABLED or ENABLED."""
    ttl = Text("vertical mode", font=SANS, font_size=13, color=MUTED)
    val = Text("ENABLED" if enabled else "DISABLED", font=MONO, font_size=17,
               color=MAGENTA if enabled else FG)
    inner = VGroup(ttl, val).arrange(DOWN, buff=0.12)
    box = RoundedRectangle(width=inner.width + 0.45, height=inner.height + 0.35,
                           corner_radius=0.1,
                           stroke_color=MAGENTA if enabled else MUTED,
                           stroke_width=2).set_fill(
                               MAGENTA if enabled else MUTED, 0.08)
    return VGroup(box.move_to(inner), inner)


def rowcol_badge():
    """Contribution-1 callback: the row & column processing card's icon."""
    inner = VGroup(
        icon_rowcol().scale(1.1),
        Text("Row & column processing", font=SANS, font_size=18, color=TEAL,
             weight="BOLD"),
    ).arrange(RIGHT, buff=0.25)
    # opaque backing + high z-index: stays readable when the split view's
    # right register file reaches into the top-right corner
    box = RoundedRectangle(width=inner.width + 0.5, height=inner.height + 0.3,
                           corner_radius=0.12, stroke_color=TEAL,
                           stroke_width=2).set_fill(BG, 0.92)
    g = VGroup(box.move_to(inner), inner)
    g.set_z_index(10)
    return g


class RowColProcessing(Scene):
    def construct(self):
        # ---- matched cut: the exact end state of Scene 6 --------------------
        st = build_arch_state()
        fp, rf_cat = st.fp, st.rf_cat
        self.add(st)
        self.wait(0.3)

        vals = cat_image(N)

        # ---- 1: a new instruction - its row/col result differs --------------
        cap1 = caption("Example of executing an instruction: vshift - it slides the pixels of a row.")
        tag = VGroup(
            Text("vshift", font=MONO, font_size=22, color=BLUE),
            Text("slide the pixels", font=SANS, font_size=16, color=MUTED),
        ).arrange(RIGHT, buff=0.25).to_corner(UP + LEFT, buff=0.5)
        tok = token("Process image: vshift").move_to(st.cpu.get_center() + UP * 0.75)
        self.play(FadeIn(cap1), FadeIn(tok), run_time=0.5)
        self.play(tok.animate.move_to(fp.dec.get_center() + UP * 0.75), run_time=0.9)
        self.play(FadeOut(tok, target_position=fp.dec), Indicate(fp.dec, color=AMBER),
                  FadeIn(tag), run_time=0.6)
        self.wait(0.6)

        # ---- 2: zoom into the Memory Register File + compute lane -----------
        cap2 = caption("Zoom in on the Memory Register File and the compute lane.")
        rf_s = RF_W / fp.rf.top.width
        plane_off = (fp.rf.top.get_center() - fp.rf.get_center()) * rf_s
        lane_grp = VGroup(fp.lanes_box, fp.lanes)
        lane_s = LANE_W / fp.lanes_box.width
        self.play(FadeOut(cap1), FadeIn(cap2),
                  FadeOut(VGroup(st.bonsai, st.bons_lbl, fp.dec, fp.dec_lbl,
                                 fp.lsu, fp.lsu_lbl,
                                 fp.lanes_lbl, fp.arrows, st.instr3, st.instr3_l,
                                 st.data3, st.cpu_grp, st.scratch, st.gm_lbl,
                                 st.cat)),
                  fp.rf.animate.scale(rf_s).move_to(RF_BIG),
                  rf_cat.animate.scale(rf_s).move_to(RF_BIG + plane_off),
                  fp.rf_lbl.animate.move_to(_p(0, 3.55)),
                  lane_grp.animate.scale(lane_s).move_to(LANE_BIG),
                  run_time=1.3)

        a_down = Arrow(_p(-RF_W * 0.3, fp.rf.get_bottom()[1] - 0.05),
                       _p(-RF_W * 0.3, fp.lanes_box.get_top()[1] + 0.05),
                       color=BLUE, buff=0, stroke_width=3,
                       max_tip_length_to_length_ratio=0.25)
        a_up = Arrow(_p(RF_W * 0.3, fp.lanes_box.get_top()[1] + 0.05),
                     _p(RF_W * 0.3, fp.rf.get_bottom()[1] - 0.05),
                     color=GREEN, buff=0, stroke_width=3,
                     max_tip_length_to_length_ratio=0.25)
        self.play(GrowArrow(a_down), GrowArrow(a_up), run_time=0.6)
        self.wait(0.5)

        # ---- 3: vertical mode DISABLED - row access; badge pins -------------
        cap3 = caption("A config bit picks the access direction. Disabled = work on ROWS.")
        mode_l = mode_box(enabled=False).move_to(MODE_FULL)
        # badge spawns big in the CENTRE (contribution callback), then docks
        badge = rowcol_badge().move_to(_p(0, 0)).scale(1.35)
        self.play(FadeIn(badge, scale=1.2), run_time=0.6)
        self.wait(0.8)
        self.play(badge.animate.scale(1 / 1.35).to_corner(UP + RIGHT, buff=0.5),
                  FadeOut(cap2), FadeIn(cap3), FadeIn(mode_l, shift=UP * 0.2),
                  run_time=0.8)
        # stress the mode: DISABLED = row access
        self.play(Indicate(mode_l, color=TEAL, scale_factor=1.18), run_time=0.6)
        row_cells = [rf_cat.cell(ROW_IDX, c) for c in range(N)]
        hl = SurroundingRectangle(VGroup(*row_cells), color=TEAL, buff=0.02,
                                  stroke_width=5.5)
        self.play(Create(hl), run_time=0.5)
        self.wait(0.5)

        # ---- 4: the row loads into the lane, chunk by chunk ------------------
        cap4 = caption("The row streams into the compute lane, chunk by chunk...")
        strip = VGroup(*[sq.copy() for sq in row_cells])
        cell_w = LANE_STRIP_W / N
        lane_c = fp.lanes_box.get_center()
        s = cell_w / strip[0].width

        def lane_slot(c):
            return _p(lane_c[0] - LANE_STRIP_W / 2 + (c + 0.5) * cell_w, lane_c[1])

        self.add(strip)
        self.play(FadeOut(cap3), FadeIn(cap4), run_time=0.4)
        chunk = N // 3
        for k in range(3):
            self.play(*[strip[c].animate.scale(s).move_to(lane_slot(c))
                        for c in range(k * chunk, (k + 1) * chunk)],
                      run_time=0.45)

        # ---- 5: process - the pixels slide -----------------------------------
        cap5 = caption("...gets processed - every pixel slides along the row...")
        rolled_row = [vals[ROW_IDX, (c - SHIFT) % N] for c in range(N)]
        self.play(FadeOut(cap4), FadeIn(cap5),
                  Indicate(fp.lanes_box, color=GREEN, scale_factor=1.03),
                  *[strip[c].animate.set_fill(rgb_to_color(rolled_row[c]))
                    for c in range(N)],
                  run_time=0.9)

        # ---- 6: store back; the sweep continues row by row -------------------
        cap6 = caption("...and is stored back. Then the next row, and the next...")
        self.play(FadeOut(cap5), FadeIn(cap6),
                  *[strip[c].animate.scale(1 / s).move_to(row_cells[c].get_center())
                    for c in range(N)],
                  run_time=0.7)
        for c in range(N):
            row_cells[c].set_fill(rgb_to_color(rolled_row[c]))
        self.remove(*strip)

        cell_h = rf_cat.cell(0, 0).height
        for _ in range(3):
            self.play(hl.animate.shift(DOWN * cell_h), run_time=0.2)
        row_done = PixelGrid(np.roll(vals, SHIFT, axis=1),
                             cell_size=rf_cat.cell(0, 0).width)
        row_done.move_to(rf_cat.get_center())
        self.play(ReplacementTransform(rf_cat, row_done), FadeOut(hl), run_time=0.9)
        self.wait(0.8)

        # ---- 7: split screen - same instruction, vertical mode ENABLED ------
        cap7 = caption("Same image, same vshift - now with vertical mode ENABLED.")
        divider = DashedLine(_p(0, 3.3), _p(0, -3.2), stroke_width=2,
                             dash_length=0.15).set_stroke(MUTED, opacity=0.7)
        left_grp = VGroup(fp.rf, fp.rf_lbl, row_done, fp.lanes_box, fp.lanes,
                          a_down, a_up)
        self.play(FadeOut(cap6), FadeIn(cap7),
                  left_grp.animate.shift(LEFT * SPLIT_DX),
                  mode_l.animate.move_to(MODE_L),
                  Create(divider), run_time=1.0)

        rf_r = fp.rf.copy().shift(RIGHT * 2 * SPLIT_DX)
        rf_lbl_r = fp.rf_lbl.copy().shift(RIGHT * 2 * SPLIT_DX)
        lane_r = VGroup(fp.lanes_box.copy(), fp.lanes.copy()).shift(RIGHT * 2 * SPLIT_DX)
        a_down_r = a_down.copy().shift(RIGHT * 2 * SPLIT_DX)
        a_up_r = a_up.copy().shift(RIGHT * 2 * SPLIT_DX)
        cat_r = PixelGrid(vals, cell_size=row_done.cell(0, 0).width)
        cat_r.move_to(row_done.get_center() + RIGHT * 2 * SPLIT_DX)
        mode_r = mode_box(enabled=True).move_to(MODE_R)
        self.play(TransformFromCopy(fp.rf, rf_r),
                  TransformFromCopy(VGroup(fp.lanes_box, fp.lanes), lane_r),
                  FadeIn(rf_lbl_r), FadeIn(a_down_r), FadeIn(a_up_r),
                  FadeIn(cat_r), FadeIn(mode_r, shift=UP * 0.2), run_time=1.0)
        # stress the flipped mode: ENABLED = column access
        self.play(Indicate(mode_r, color=MAGENTA, scale_factor=1.18), run_time=0.6)
        self.wait(0.4)

        # ---- 8: a COLUMN loads into the lane (transposed look) ---------------
        cap8 = caption("Now a COLUMN streams into the same horizontal lane...")
        col_cells = [cat_r.cell(r, COL_IDX) for r in range(N)]
        hl_c = SurroundingRectangle(VGroup(*col_cells), color=MAGENTA, buff=0.02,
                                    stroke_width=5.5)
        self.play(FadeOut(cap7), FadeIn(cap8), Create(hl_c), run_time=0.6)

        strip_c = VGroup(*[sq.copy() for sq in col_cells])
        lane_rc = lane_r[0].get_center()
        s_c = cell_w / strip_c[0].width

        def lane_slot_r(i):
            return _p(lane_rc[0] - LANE_STRIP_W / 2 + (i + 0.5) * cell_w, lane_rc[1])

        self.add(strip_c)
        for k in range(3):
            self.play(*[strip_c[r].animate.scale(s_c).move_to(lane_slot_r(r))
                        for r in range(k * chunk, (k + 1) * chunk)],
                      run_time=0.45)

        # ---- 9-10: process the column and store it back ----------------------
        cap9 = caption("...the pixels slide along the COLUMN, and store back.")
        rolled_col = [vals[(r - SHIFT) % N, COL_IDX] for r in range(N)]
        self.play(FadeOut(cap8), FadeIn(cap9),
                  Indicate(lane_r[0], color=MAGENTA, scale_factor=1.03),
                  *[strip_c[r].animate.set_fill(rgb_to_color(rolled_col[r]))
                    for r in range(N)],
                  run_time=0.9)
        self.play(*[strip_c[r].animate.scale(1 / s_c).move_to(col_cells[r].get_center())
                    for r in range(N)],
                  run_time=0.7)
        for r in range(N):
            col_cells[r].set_fill(rgb_to_color(rolled_col[r]))
        self.remove(*strip_c)

        for _ in range(3):
            self.play(hl_c.animate.shift(RIGHT * cell_h), run_time=0.2)
        col_done = PixelGrid(np.roll(vals, SHIFT, axis=0),
                             cell_size=cat_r.cell(0, 0).width)
        col_done.move_to(cat_r.get_center())
        self.play(ReplacementTransform(cat_r, col_done), FadeOut(hl_c), run_time=0.9)

        # ---- 11: the payoff - one instruction, two outcomes ------------------
        # blink the MODE boxes and the row/column highlights (not the planes):
        # the mode bit is what made the difference
        cap11 = caption("One instruction, one bit flipped: rows slide sideways, columns slide down.")
        hl2 = SurroundingRectangle(
            VGroup(*[row_done.cell(ROW_IDX, c) for c in range(N)]),
            color=TEAL, buff=0.02, stroke_width=5.5)
        hlc2 = SurroundingRectangle(
            VGroup(*[col_done.cell(r, COL_IDX) for r in range(N)]),
            color=MAGENTA, buff=0.02, stroke_width=5.5)
        blink = VGroup(mode_l, mode_r, hl2, hlc2)
        self.play(FadeOut(cap9), FadeIn(cap11), FadeIn(hl2), FadeIn(hlc2),
                  run_time=0.7)
        for _ in range(2):
            self.play(FadeOut(blink), run_time=0.22)
            self.play(FadeIn(blink), run_time=0.22)
        self.wait(1.6)

        # fade everything - Scene 7.5 (heterogeneous processing) starts fresh
        rest = VGroup(fp.rf, fp.rf_lbl, row_done, fp.lanes_box, fp.lanes,
                      a_down, a_up, mode_l, mode_r, divider, rf_r, rf_lbl_r,
                      lane_r, a_down_r, a_up_r, col_done, hl2, hlc2,
                      tag, badge, cap11)
        self.play(FadeOut(rest), run_time=0.8)
        self.wait(0.2)
