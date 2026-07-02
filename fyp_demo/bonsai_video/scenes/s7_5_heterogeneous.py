"""Scene 7.5 - Heterogeneous processing, following
drawio_scene_ref/hetergeneous_processing.drawio (19 numbered sub-scene
frames). Placed between Scene 7 (row/col) and Scene 8 (32 registers).
Three example panels show different instruction types acting on a whole
image plane, one instruction each:

  panel 1 (left)   - vshift right: the whole plane slides right (horizontal
                     mode), then slides down (vertical mode) - cumulative.
  panel 2 (centre) - load/store in vertical mode: the buffer is still read
                     row by row, but the register fills column by column,
                     so the image lands TRANSPOSED - a free transpose.
  panel 3 (right)  - vgather: pixels are rearranged by an index register.
                     Horizontal mode mirrors each row; vertical mode then
                     flips the columns - together a 180-degree turn.

Then the primitive tags reveal under each panel (neighbour-to-neighbour /
CNN, free transpose / matrix calculation, global pixel op / attention) and
the "Heterogeneous processing" contribution badge lands. Hands the centre
register-file stack to Scene 8 (the zoom-into-memory handoff).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Rectangle, RoundedRectangle, Text,
    FadeIn, FadeOut, ReplacementTransform, Indicate,
    LEFT, RIGHT, UP, DOWN,
)
from common.layout import RF_POS, RF_SIZE
from common.assets import cat_image
from common.arch import make_reg_file
from common.pixel_grid import PixelGrid
from common.palette import (
    BG, FG, MUTED, TEAL, AMBER, BLUE, MAGENTA, SANS, MONO, caption,
)
from scenes.s3_6_contribution import icon_hetero

N = 24
SHIFT = 6
PANEL_X = (-4.3, 0.0, 4.3)
PANEL_Y = 0.35                          # stack centre height
PLANE = 2.7                             # register plane size per panel
BUF_POS = np.array([-1.9, 2.9, 0.0])    # panel 2's global-memory cat (top-left
                                        # of the middle section)


def _p(x, y):
    return np.array([x, y, 0.0])


def mode_text(vertical):
    return Text("vertical" if vertical else "horizontal", font=SANS,
                font_size=22, color=MAGENTA if vertical else MUTED)


def build_panel(instr, vertical, x, vals):
    """One example panel: instruction label / register stack + cat / mode."""
    stack = make_reg_file(size=PLANE).move_to(_p(x, PANEL_Y))
    plane = stack.top
    cat = PixelGrid(vals, cell_size=plane.width / N).move_to(plane.get_center())
    instr_l = Text(instr, font=MONO, font_size=21, color=BLUE)
    if instr_l.width > 3.6:
        instr_l.scale_to_fit_width(3.6)
    instr_l.next_to(stack, UP, buff=0.3)
    mode_l = mode_text(vertical).next_to(stack, DOWN, buff=0.3)
    g = VGroup(stack, cat, instr_l, mode_l)
    g.stack, g.cat, g.instr_l, g.mode_l = stack, cat, instr_l, mode_l
    return g


def hetero_badge():
    """Contribution-1 callback: the heterogeneous processing card's icon."""
    inner = VGroup(
        icon_hetero().scale(0.9),
        Text("Heterogeneous processing", font=SANS, font_size=18, color=AMBER,
             weight="BOLD"),
    ).arrange(RIGHT, buff=0.25)
    box = RoundedRectangle(width=inner.width + 0.5, height=inner.height + 0.3,
                           corner_radius=0.12, stroke_color=AMBER,
                           stroke_width=2).set_fill(BG, 0.92)
    g = VGroup(box.move_to(inner), inner)
    g.set_z_index(10)
    return g


class HeterogeneousProcessing(Scene):
    def construct(self):
        vals = cat_image(N)

        # ---- the contribution badge OPENS the scene: spawns big in the
        # centre, then docks top-right for the rest of the scene -------------
        badge = hetero_badge().move_to(_p(0, 0)).scale(1.4)
        cap0 = caption("Heterogeneous processing: one fabric runs many different kernels.")
        self.play(FadeIn(badge, scale=1.2), FadeIn(cap0), run_time=0.7)
        self.wait(1.0)
        self.play(badge.animate.scale(1 / 1.4).to_corner(UP + RIGHT, buff=0.5),
                  run_time=0.7)

        # persistent top-left note (every drawio frame carries it) - boxed
        # and bigger so it reads as the rule of the whole scene
        note_t = Text("One instruction, whole image", font=SANS, font_size=23,
                      color=AMBER, weight="BOLD")
        note_box = RoundedRectangle(width=note_t.width + 0.45,
                                    height=note_t.height + 0.3,
                                    corner_radius=0.1, stroke_color=AMBER,
                                    stroke_width=2).set_fill(BG, 0.92)
        note = VGroup(note_box.move_to(note_t), note_t).to_corner(UP + LEFT,
                                                                  buff=0.4)
        note.set_z_index(10)

        def regrid(panel, new_vals, run_time=0.9, extra=()):
            """Replace a panel's cat with new values (whole-plane result)."""
            new = PixelGrid(new_vals, cell_size=panel.cat.cell(0, 0).width)
            new.move_to(panel.cat.get_center())
            self.play(ReplacementTransform(panel.cat, new), *extra,
                      run_time=run_time)
            panel.cat = new
            panel.add(new)
            return new

        def flip_mode(panel):
            new_l = mode_text(True).move_to(panel.mode_l)
            self.play(ReplacementTransform(panel.mode_l, new_l), run_time=0.4)
            panel.mode_l = new_l

        # ---- 2-4: panel 1 - vshift right, whole plane, both directions ------
        cap2 = caption("Different instruction types, one plane at a time. vshift: the whole image slides.")
        p1 = build_panel("vshift right", vertical=False, x=PANEL_X[0], vals=vals)
        self.play(FadeOut(cap0), FadeIn(note), FadeIn(p1), FadeIn(cap2),
                  run_time=0.8)
        self.wait(0.5)
        r1 = np.roll(vals, SHIFT, axis=1)
        regrid(p1, r1)
        self.wait(0.4)
        flip_mode(p1)
        r2 = np.roll(r1, SHIFT, axis=0)
        regrid(p1, r2)
        # the mode word was only there to support the animation - retire it
        self.play(p1.mode_l.animate.set_opacity(0), run_time=0.3)
        self.wait(0.4)

        # ---- 5-6: panel 2 - vertical-mode load/store = free transpose -------
        cap5 = caption("load/store in vertical mode: read rows from the buffer, write columns...")
        p2 = build_panel("load/store", vertical=True, x=PANEL_X[1], vals=vals)
        self.remove(p2.cat)          # the cat arrives column by column instead
        buf = PixelGrid(cat_image(12), cell_size=0.11).move_to(BUF_POS)
        buf_l = Text("global memory", font=SANS, font_size=14, color=MUTED)
        buf_l.next_to(buf, RIGHT, buff=0.15)
        self.play(FadeOut(cap2), FadeIn(cap5),
                  FadeIn(VGroup(p2.stack, p2.instr_l, p2.mode_l)),
                  FadeIn(buf), FadeIn(buf_l), run_time=0.8)

        tvals = np.transpose(vals, (1, 0, 2))
        tcat = PixelGrid(tvals, cell_size=p2.stack.top.width / N)
        tcat.move_to(p2.stack.top.get_center())
        tcat.set_opacity(0.0)
        self.add(tcat)
        hover = Rectangle(width=buf.width, height=buf.height / 6,
                          stroke_width=4.5).set_stroke(TEAL, opacity=0.95).set_fill(TEAL, 0.25)
        hover.move_to(buf.get_top() + DOWN * buf.height / 12)
        self.play(FadeIn(hover), run_time=0.3)
        chunk = N // 6
        for k in range(6):
            cells = [tcat.cell(r, c) for c in range(k * chunk, (k + 1) * chunk)
                     for r in range(N)]
            anims = [sq.animate.set_opacity(1.0) for sq in cells]
            if k > 0:
                anims.append(hover.animate.shift(DOWN * buf.height / 6))
            self.play(*anims, run_time=0.35)
        p2.cat = tcat
        p2.add(tcat)
        cap6 = caption("...and the image lands transposed - for free.")
        self.play(FadeOut(hover), FadeOut(buf), FadeOut(buf_l),
                  FadeOut(cap5), FadeIn(cap6), run_time=0.6)
        self.play(p2.mode_l.animate.set_opacity(0), run_time=0.3)
        self.wait(0.5)

        # ---- 7-10: panel 3 - vgather PHYSICALLY moves pixels within rows ----
        cap7 = caption("vgather: an index register rearranges pixels - here, mirroring every row.")
        p3 = build_panel("vgather (Global pixel rearrange)", vertical=False,
                         x=PANEL_X[2], vals=vals)
        self.play(FadeOut(cap6), FadeIn(cap7), FadeIn(p3), run_time=0.8)

        def swap_grid(panel, new_vals):
            """After cells have physically moved, swap in a clean grid
            (identical look) so cell indexing matches positions again."""
            ng = PixelGrid(new_vals, cell_size=panel.cat.cell(0, 0).width)
            ng.move_to(panel.cat.get_center())
            self.add(ng)
            self.remove(panel.cat)
            panel.remove(panel.cat)
            panel.cat = ng
            panel.add(ng)

        def mirror_rows(rs, arc):
            """Each pixel of rows `rs` travels to its mirrored slot."""
            anims = []
            for r in rs:
                cells = [p3.cat.cell(r, c) for c in range(N)]
                tg = [cells[N - 1 - c].get_center().copy() for c in range(N)]
                anims += [cells[c].animate(path_arc=arc).move_to(tg[c])
                          for c in range(N)]
            return anims

        row_hover = Rectangle(width=PLANE, height=PLANE / 6, stroke_width=2)
        row_hover.set_stroke(TEAL, opacity=0.9).set_fill(TEAL, 0.18)
        row_hover.move_to(p3.cat.get_top() + DOWN * PLANE / 12)
        self.play(FadeIn(row_hover), run_time=0.3)
        self.play(*mirror_rows(range(0, 4), 1.0), run_time=0.9)
        self.play(row_hover.animate.shift(DOWN * PLANE / 6), run_time=0.25)
        self.play(*mirror_rows(range(4, 8), 1.0), run_time=0.9)
        self.play(*mirror_rows(range(8, N), 0.5), FadeOut(row_hover),
                  run_time=1.1)
        swap_grid(p3, vals[:, ::-1])
        self.wait(0.5)

        # ---- 11-13: vgather in vertical mode - columns physically flip ------
        cap11 = caption("Flip the mode: the same gather now rearranges along columns.")
        flip_mode(p3)

        def flip_cols(cs, arc):
            anims = []
            for c in cs:
                cells = [p3.cat.cell(r, c) for r in range(N)]
                tg = [cells[N - 1 - r].get_center().copy() for r in range(N)]
                anims += [cells[r].animate(path_arc=arc).move_to(tg[r])
                          for r in range(N)]
            return anims

        col_hover = Rectangle(width=PLANE / 6, height=PLANE, stroke_width=2)
        col_hover.set_stroke(MAGENTA, opacity=0.9).set_fill(MAGENTA, 0.18)
        col_hover.move_to(p3.cat.get_left() + RIGHT * PLANE / 12)
        self.play(FadeOut(cap7), FadeIn(cap11), FadeIn(col_hover), run_time=0.5)
        self.play(*flip_cols(range(0, 4), 1.0), run_time=0.9)
        self.play(col_hover.animate.shift(RIGHT * PLANE / 6), run_time=0.25)
        self.play(*flip_cols(range(4, 8), 1.0), run_time=0.9)
        self.play(*flip_cols(range(8, N), 0.5), FadeOut(col_hover),
                  run_time=1.1)
        swap_grid(p3, vals[::-1, ::-1])
        self.play(p3.mode_l.animate.set_opacity(0), run_time=0.3)
        self.wait(0.5)

        # ---- 14-19: the primitives these unlock, panel by panel -------------
        cap14 = caption("Together these are the building blocks of real vision kernels.")

        def desc_tag(x, text, color):
            t = Text(text, font=SANS, font_size=20, color=color)
            if t.width > 3.3:
                t.scale_to_fit_width(3.3)
            return t.move_to(_p(x, -2.05))

        def hero_tag(x, text, color):
            """The important kernel classes: big, bold, boxed."""
            t = Text(text, font=SANS, font_size=19, color=color, weight="BOLD")
            if t.width > 3.3:
                t.scale_to_fit_width(3.3)
            box = RoundedRectangle(width=max(t.width + 0.45, 2.6), height=0.6,
                                   corner_radius=0.1, stroke_color=color,
                                   stroke_width=2.5).set_fill(color, 0.12)
            return VGroup(box.move_to(t), t).move_to(_p(x, -2.72))

        d1 = desc_tag(PANEL_X[0], "Neighbour-to-neighbour", TEAL)
        d2 = desc_tag(PANEL_X[1], "Free transpose", MAGENTA)
        d3 = desc_tag(PANEL_X[2], "Global pixel operation", BLUE)
        h1 = hero_tag(PANEL_X[0], "CNN primitives", TEAL)
        h2 = hero_tag(PANEL_X[1], "Matrix calculation", MAGENTA)
        h3 = hero_tag(PANEL_X[2], "Attention primitives", BLUE)
        self.play(FadeOut(cap11), FadeIn(cap14),
                  FadeIn(d1), FadeIn(d2), FadeIn(d3), run_time=0.6)
        # the important ones each get their own entrance
        for h, c in ((h1, TEAL), (h2, MAGENTA), (h3, BLUE)):
            self.play(FadeIn(h, scale=1.4), run_time=0.5)
            self.play(Indicate(h, color=c, scale_factor=1.08), run_time=0.35)
        self.wait(0.6)

        capb = caption("One fabric, many kernels - heterogeneous processing.")
        self.play(FadeOut(cap14), FadeIn(capb),
                  Indicate(badge, color=AMBER, scale_factor=1.1), run_time=0.8)
        self.wait(1.6)

        # ---- handoff: the centre stack carries into Scene 8 ------------------
        rest = VGroup(note, badge, capb, d1, d2, d3, h1, h2, h3,
                      p1, p3, p2.cat, p2.instr_l, p2.mode_l)
        self.play(FadeOut(rest), run_time=0.7)
        self.play(p2.stack.animate.scale(RF_SIZE / p2.stack.top.width).move_to(RF_POS),
                  run_time=0.5)
        self.wait(0.2)
