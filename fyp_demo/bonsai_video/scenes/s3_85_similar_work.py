"""Scene 3.85 - BONSAI's position relative to similar RISC-V extensions,
following drawio_scene_ref/similar_matrix_work.drawio (8 numbered frames).
Placed between the contributions interlude and "How does a BONSAI Processor
work?".

One diagram morphs through the variants (objects dragged, never respawned):
  1.   title card.
  2.   base vector processor: 1D vector registers (x32 depth), a compute
       lane, CPU + global memory below, and an EMPTY dotted attachment slot.
  3.   IME: the slot becomes a Matrix ALU.
  4.   VME: the slot becomes one 2D matrix accumulation register.
  5.   AME: the slot becomes a whole matrix engine (tiles + ALU + state).
  6.   BONSAI: the slot empties; the 1D registers stretch UP into 2D squares
       (width = height).
  7.   the compute lane duplicates for throughput.
  8.   highlights: multiple square registers + multiple compute lanes.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Square, Rectangle, RoundedRectangle, Text, Arrow,
    DoubleArrow, DashedVMobject, SurroundingRectangle,
    FadeIn, FadeOut, Create, ReplacementTransform,
    LEFT, RIGHT, UP, DOWN,
)
from common.icons import chip
from common.palette import (
    FG, MUTED, TEAL, AMBER, BLUE, GREEN, MAGENTA, PURPLE, SANS, caption,
)

REG_C = np.array([-1.3, 1.5, 0.0])      # register stack centre
LANE_C = np.array([-1.3, -0.6, 0.0])    # compute lane centre
ATT_C = np.array([3.9, -0.6, 0.0])      # attachment slot centre
CPU_C = np.array([-2.6, -2.5, 0.0])
GM_C = np.array([0.0, -2.5, 0.0])
HEAD_Y = 2.62


def _p(x, y):
    return np.array([x, y, 0.0])


def depth_tag(front, text="32 registers"):
    """Small diagonal depth arrow at the stack's right edge + count label
    (points rightward so it never reaches into the header above)."""
    p1 = front.get_corner(UP + RIGHT) + _p(0.08, 0.02)
    p2 = p1 + _p(0.6, 0.25)
    a = Arrow(p1, p2, color=AMBER, buff=0, stroke_width=3,
              max_tip_length_to_length_ratio=0.3)
    t = Text(text, font=SANS, font_size=15,
             color=AMBER).next_to(p2, RIGHT, buff=0.06)
    return VGroup(a, t)


def stack_1d():
    """Three offset thin rects: the 1D vector register file (front = [-1])."""
    g = VGroup()
    for i in (2, 1, 0):                       # back to front
        r = Rectangle(width=4.2, height=0.6, stroke_color=BLUE,
                      stroke_width=2.2).set_fill(BLUE, 0.10)
        r.set_stroke(opacity=0.45 + 0.55 * (2 - i) / 2)
        r.shift(_p(0.17 * i, 0.14 * i))
        g.add(r)
    g.move_to(REG_C)
    g.front = g[-1]
    return g


def stack_2d():
    """The same three registers stretched up into squares (width = height)."""
    g = VGroup()
    for i in (2, 1, 0):
        r = Square(1.7, stroke_color=BLUE, stroke_width=2.2).set_fill(BLUE, 0.10)
        r.set_stroke(opacity=0.45 + 0.55 * (2 - i) / 2)
        r.shift(_p(0.14 * i, 0.12 * i))
        g.add(r)
    g.move_to(REG_C + DOWN * 0.1)
    g.front = g[-1]
    return g


def lane_box():
    box = RoundedRectangle(width=4.2, height=0.7, corner_radius=0.08,
                           stroke_color=GREEN, stroke_width=2.2).set_fill(GREEN, 0.06)
    t = Text("Compute lane", font=SANS, font_size=18, color=GREEN).move_to(box)
    g = VGroup(box, t)
    g.box = box
    return g


def header(text, color=FG):
    return Text(text, font=SANS, font_size=28, color=color,
                weight="BOLD").move_to(_p(0, HEAD_Y))


def att_empty():
    box = DashedVMobject(Rectangle(width=2.3, height=1.7),
                         num_dashes=40).set_stroke(MUTED, width=2, opacity=0.8)
    t = Text("no matrix\nhardware", font=SANS, font_size=15, color=MUTED,
             line_spacing=0.85).move_to(box)
    return VGroup(box, t).move_to(ATT_C)


def att_ime():
    return chip("Matrix\nALU", w=1.7, h=1.3, color=AMBER,
                font_size=18).move_to(ATT_C)


def att_vme():
    g = chip("2D Matrix\nAccumulation\nRegister", w=2.3, h=1.9, color=MAGENTA,
             font_size=15).move_to(ATT_C)
    n = Text("only 1", font=SANS, font_size=14,
             color=MUTED).next_to(g, DOWN, buff=0.12)
    return VGroup(g, n)


def att_ame():
    box = RoundedRectangle(width=2.9, height=2.6, corner_radius=0.1,
                           stroke_color=AMBER, stroke_width=2.4).set_fill(AMBER, 0.04)
    box.move_to(ATT_C)
    ttl = Text("Matrix Engine", font=SANS, font_size=15, color=AMBER,
               weight="BOLD").move_to(box.get_top() + DOWN * 0.25)
    tiles = VGroup()
    for i in (2, 1, 0):
        s = Square(0.85, stroke_color=MAGENTA, stroke_width=1.8).set_fill(MAGENTA, 0.10)
        s.shift(_p(0.12 * i, 0.10 * i))
        tiles.add(s)
    tiles.move_to(box.get_center() + UP * 0.15 + LEFT * 0.45)
    t_lbl = Text("2D Matrix\nTiles", font=SANS, font_size=11, color=MAGENTA,
                 line_spacing=0.85).next_to(tiles, RIGHT, buff=0.18)
    alu = chip("Matrix ALU", w=1.15, h=0.5, color=FG, font_size=11)
    st = chip("Matrix state", w=1.15, h=0.5, color=FG, font_size=11)
    VGroup(alu, st).arrange(RIGHT, buff=0.15).move_to(box.get_bottom() + UP * 0.45)
    return VGroup(box, ttl, tiles, t_lbl, alu, st)


class SimilarWork(Scene):
    def construct(self):
        # ---- 1: title card ---------------------------------------------------
        title = Text("BONSAI's Position Relative to Similar RISC-V Extensions",
                     font=SANS, font_size=38, color=FG, weight="BOLD")
        if title.width > 13.0:
            title.scale_to_fit_width(13.0)
        self.play(FadeIn(title, shift=UP * 0.2), run_time=0.8)
        self.wait(1.6)
        self.play(title.animate.scale(0.62).to_edge(UP, buff=0.35), run_time=0.8)

        # ---- 2: the base vector processor ------------------------------------
        head = header("Base vector processor")
        regs = stack_1d()
        regs_lbl = Text("1D vector registers", font=SANS, font_size=18,
                        color=BLUE).move_to(regs.front)
        depth = depth_tag(regs.front)
        lane = lane_box().move_to(LANE_C)
        a_dn = Arrow(_p(-2.2, regs.get_bottom()[1] - 0.05),
                     _p(-2.2, lane.box.get_top()[1] + 0.05), color=BLUE, buff=0,
                     stroke_width=3, max_tip_length_to_length_ratio=0.2)
        a_up = Arrow(_p(-0.4, lane.box.get_top()[1] + 0.05),
                     _p(-0.4, regs.get_bottom()[1] - 0.05), color=GREEN, buff=0,
                     stroke_width=3, max_tip_length_to_length_ratio=0.2)
        cpu = chip("CPU", w=1.2, h=0.9, color=MUTED).move_to(CPU_C)
        gm = chip("Global\nMemory", w=1.6, h=0.9, color=GREEN,
                  font_size=15).move_to(GM_C)
        a_cpu = DoubleArrow(_p(CPU_C[0], lane.box.get_bottom()[1] - 0.05),
                            cpu.box.get_top(), color=MUTED, buff=0.05,
                            stroke_width=2.5, max_tip_length_to_length_ratio=0.15)
        a_gm = DoubleArrow(_p(GM_C[0], lane.box.get_bottom()[1] - 0.05),
                           gm.box.get_top(), color=GREEN, buff=0.05,
                           stroke_width=2.5, max_tip_length_to_length_ratio=0.15)
        att = att_empty()
        a_att = DoubleArrow(_p(0.85, -0.6), _p(2.6, -0.6), color=MUTED,
                            buff=0, stroke_width=2.5,
                            max_tip_length_to_length_ratio=0.12)

        cap1 = caption("Similar RISC-V extensions start from the same base vector processor.")
        self.play(FadeIn(head), FadeIn(regs), FadeIn(regs_lbl), FadeIn(depth),
                  FadeIn(cap1), run_time=0.9)
        self.play(FadeIn(lane), Create(a_dn), Create(a_up),
                  FadeIn(cpu), FadeIn(gm), Create(a_cpu), Create(a_gm),
                  run_time=0.9)
        self.play(Create(a_att), FadeIn(att), run_time=0.7)
        self.wait(1.6)

        # ---- 3-5: the slot morphs through IME / VME / AME --------------------
        variants = (
            ("Integrated Matrix Extension (IME)", AMBER, att_ime(),
             "IME: a matrix ALU bolted onto the compute lane."),
            ("Vector-Matrix Extension (VME)", MAGENTA, att_vme(),
             "VME: one dedicated 2D accumulation register."),
            ("Attached Matrix Extension (AME)", AMBER, att_ame(),
             "AME: a whole separate matrix engine attached."),
        )
        cap = cap1
        for name, col, new_att, cap_text in variants:
            new_head = header(name, col)
            new_cap = caption(cap_text)
            self.play(ReplacementTransform(head, new_head),
                      ReplacementTransform(att, new_att),
                      FadeOut(cap), FadeIn(new_cap), run_time=0.9)
            head, att, cap = new_head, new_att, new_cap
            self.wait(1.8)

        # ---- 6: BONSAI - no attachment; the registers themselves go 2D -------
        # the header docks left so the growing square stack keeps the centre
        head_b = header("BONSAI", PURPLE).move_to(_p(-4.9, 2.0))
        cap_b = caption("BONSAI attaches nothing. The vector registers themselves become 2D squares.")
        self.play(ReplacementTransform(head, head_b),
                  FadeOut(att), FadeOut(a_att),
                  FadeOut(cap), FadeIn(cap_b), run_time=0.9)

        sq = stack_2d()
        sq_lbl = Text("2D square\nregisters", font=SANS, font_size=16,
                      color=BLUE, line_spacing=0.85).move_to(sq.front)
        depth2 = depth_tag(sq.front)
        a_dn2 = Arrow(_p(-2.2, sq.get_bottom()[1] - 0.05),
                      _p(-2.2, lane.box.get_top()[1] + 0.05), color=BLUE, buff=0,
                      stroke_width=3, max_tip_length_to_length_ratio=0.2)
        a_up2 = Arrow(_p(-0.4, lane.box.get_top()[1] + 0.05),
                      _p(-0.4, sq.get_bottom()[1] - 0.05), color=GREEN, buff=0,
                      stroke_width=3, max_tip_length_to_length_ratio=0.2)
        self.play(ReplacementTransform(regs, sq),
                  ReplacementTransform(regs_lbl, sq_lbl),
                  ReplacementTransform(depth, depth2),
                  ReplacementTransform(a_dn, a_dn2),
                  ReplacementTransform(a_up, a_up2), run_time=1.2)
        # width = height annotations: width along the TOP edge (the space
        # below belongs to the lane), height along the left edge
        f = sq.front
        w_a = DoubleArrow(f.get_corner(UP + LEFT) + UP * 0.18,
                          f.get_corner(UP + RIGHT) + UP * 0.18, color=TEAL,
                          buff=0, stroke_width=3, tip_length=0.15)
        h_a = DoubleArrow(f.get_corner(DOWN + LEFT) + LEFT * 0.18,
                          f.get_corner(UP + LEFT) + LEFT * 0.18, color=TEAL,
                          buff=0, stroke_width=3, tip_length=0.15)
        wh = Text("width = height", font=SANS, font_size=16,
                  color=TEAL).next_to(w_a, UP, buff=0.1)
        self.play(FadeIn(w_a), FadeIn(h_a), FadeIn(wh), run_time=0.6)
        self.wait(1.6)

        # ---- 7: the compute lane duplicates -----------------------------------
        cap_l = caption("And the compute lanes duplicate for more throughput.")
        lane2 = lane.copy()
        lane2.shift(DOWN * 0.85)
        dots = Text("·  ·  ·", font=SANS, font_size=26,
                    color=MUTED).move_to(_p(-1.3, -1.8))
        self.play(FadeOut(cap_b), FadeIn(cap_l),
                  FadeIn(lane2, shift=DOWN * 0.3), FadeIn(dots), run_time=0.9)
        self.wait(1.4)

        # ---- 8: highlights - the two things that make BONSAI different -------
        cap_e = caption("The base processor itself goes 2D. That is BONSAI's position.")
        hl_r = SurroundingRectangle(VGroup(sq, sq_lbl, w_a, h_a, wh),
                                    color=TEAL, buff=0.15,
                                    corner_radius=0.1).set_stroke(TEAL, 3.5)
        hl_r_l = Text("Multiple square registers", font=SANS, font_size=17,
                      color=TEAL, weight="BOLD").next_to(hl_r, RIGHT, buff=0.3)
        hl_l = SurroundingRectangle(VGroup(lane, lane2, dots), color=GREEN,
                                    buff=0.12, corner_radius=0.1).set_stroke(GREEN, 3.5)
        hl_l_l = Text("Multiple compute lanes", font=SANS, font_size=17,
                      color=GREEN, weight="BOLD").next_to(hl_l, RIGHT, buff=0.3)
        self.play(FadeOut(cap_l), FadeIn(cap_e),
                  Create(hl_r), FadeIn(hl_r_l), run_time=0.8)
        self.play(Create(hl_l), FadeIn(hl_l_l), run_time=0.8)
        self.wait(2.2)

        self.play(FadeOut(VGroup(title, head_b, sq, sq_lbl, depth2, a_dn2,
                                 a_up2, w_a, h_a, wh, lane, lane2, dots, cpu,
                                 gm, a_cpu, a_gm, hl_r, hl_r_l, hl_l, hl_l_l,
                                 cap_e)), run_time=0.8)
