"""Scene 6 - Programmability, following drawio_scene_ref/programmability.drawio
(16 numbered sub-scene frames). Continues seamlessly from Scene 3.10 (SoC
dataflow) via a matched cut: it re-adds that scene's exact end state and
REWINDS it, then zooms into the Bonsai processor to tell the programmability
story, and finishes by loading the cat image row by row into the
Memory Register File (absorbing the old Scene 5 animation). Every object is
dragged, never respawned.

Sub-scenes (drawio frames):
  1-4.  rewind "with some effect": result leaves the CPU, turns back into the
        cat, and the cat returns to the scratchpad memory.
  5.    the CPU re-sends Instructions - how does this programming work?
  6.    zoom in: Bonsai enlarges to fill the frame; the CPU parks at the left
        edge, the scratchpad (holding the cat) at the right edge.
  7.    Bonsai internals appear: decoder, compute lane, LSU, and the
        Memory Register File; instructions now point at the decoder and the
        scratchpad feeds the LSU.
  8.    an instruction token travels CPU -> decoder.
  9-10. zoom into the decoder: it speaks RISC-V RVV vector instructions
        (vadd / vmul / vshift / vgather on registers v0, v1, ...).
  11-13. LLVM/GNU open-source stock compiler: instruction rows compile into
        machine code one by one.
  14.   zoom back out to the architecture.
  15.   the first real instruction arrives: "Load Image".
  16.   the cat streams row by row scratchpad -> LSU -> compute lane -> up
        into the Memory Register File (the Scene 5 animation, same geometry).

Ends with the loaded architecture LEFT ON SCREEN: Scene 7 (row/column
processing) re-adds the exact same state via `build_arch_state()` and carries
on (matched cut). The register-file handoff to Scene 8 now lives in Scene 7.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Square, Rectangle, Text, Arrow, VMobject, RoundedRectangle,
    FadeIn, FadeOut, GrowArrow, ReplacementTransform, Indicate, Flash,
    MoveAlongPath, Succession, LaggedStart, rgb_to_color,
    LEFT, RIGHT, UP, DOWN, ORIGIN,
)
from common.layout import make_cat
from common.assets import cat_image
from common.arch import bonsai_floorplan
from common.palette import (
    BG, FG, MUTED, TEAL, AMBER, BLUE, GREEN, PURPLE, SANS, MONO, caption,
)
from scenes.s3_10_soc_dataflow import (
    build_end_state, build_pipeline, CAT_IN_BLOCK_W,
)

# --- zoomed layout (drawio frames 6-8, 14-16) --------------------------------
BONSAI_BIG_SCALE = 4.4                 # 1.5x1.3 chip -> ~6.6x5.7 box
BONSAI_BIG_POS = np.array([0.0, 0.1, 0.0])
CPU_EDGE = np.array([-5.75, -2.0, 0.0])
SCRATCH_EDGE = np.array([5.6, -2.0, 0.0])
FP_SCALE = 0.72                        # floorplan fitted inside the big box
DEC_ZOOM = 5.8                         # frames 9-13 decoder close-up
ROWS = 24                              # row-by-row load resolution (as Scene 5)


def _p(x, y):
    return np.array([x, y, 0.0])


def token(text):
    """A small travelling instruction token on the blue channel."""
    return Text(text, font=MONO, font_size=15, color=BLUE)


def build_arch_state():
    """The exact final frame of this scene (zoomed architecture with the cat
    loaded in the Memory Register File) - Scene 7 re-adds this for the
    matched cut. Construction mirrors the animated path of `construct`."""
    p = build_pipeline()
    bonsai = p.bonsai
    bonsai.scale(BONSAI_BIG_SCALE).move_to(BONSAI_BIG_POS)
    bonsai.label.set_opacity(0)
    cpu_grp = p.cpu_grp.move_to(CPU_EDGE)
    cat = make_cat(n=24).scale_to_fit_width(CAT_IN_BLOCK_W).move_to(p.scratch)
    VGroup(p.scratch, cat).move_to(SCRATCH_EDGE)
    fp = bonsai_floorplan(scale=FP_SCALE)
    fp.move_to(BONSAI_BIG_POS)
    instr3 = Arrow(p.cpu.get_right(), fp.dec.get_left(), color=BLUE, buff=0.15,
                   stroke_width=3.5, max_tip_length_to_length_ratio=0.14)
    instr3_l = Text("Instructions", font=SANS, font_size=15,
                    color=BLUE).next_to(instr3.get_center(), UP + LEFT, buff=0.1)
    data3 = Arrow(p.scratch.get_left(), fp.lsu.get_right(), color=GREEN,
                  buff=0.15, stroke_width=3.5, max_tip_length_to_length_ratio=0.14)
    bons_lbl = Text("BONSAI Processor", font=SANS, font_size=26,
                    color=PURPLE, weight="BOLD")
    bons_lbl.move_to(bonsai.box.get_top() + DOWN * 0.38)
    gm_lbl = Text("Global Memory", font=SANS, font_size=14, color=GREEN)
    gm_lbl.next_to(p.scratch, DOWN, buff=0.15)
    plane = fp.rf.top
    rf_cat = make_cat(n=24)
    rf_cat.scale_to_fit_width(plane.width).move_to(plane.get_center())
    g = VGroup(bonsai, bons_lbl, fp, instr3, instr3_l, data3, cpu_grp,
               p.scratch, gm_lbl, cat, rf_cat)
    g.bonsai, g.bons_lbl, g.fp = bonsai, bons_lbl, fp
    g.gm_lbl = gm_lbl
    g.instr3, g.instr3_l, g.data3 = instr3, instr3_l, data3
    g.cpu_grp, g.cpu = cpu_grp, p.cpu
    g.scratch, g.cat, g.rf_cat = p.scratch, cat, rf_cat
    return g


class Programmability(Scene):
    def construct(self):
        # ---- matched cut: the exact end state of Scene 3.10 -----------------
        st = build_end_state()
        p = st.pipe
        sensor, scratch, bonsai, cpu = p.sensor, p.scratch, p.bonsai, p.cpu
        self.add(st)
        self.wait(0.3)

        # ---- 1-4: rewind back to "the cat sits in memory" -------------------
        # the whole pipeline dims and a big overlay explains: we're going
        # again, this time with details - so nobody wonders why things are
        # travelling backwards
        dim = Rectangle(width=15, height=9, stroke_width=0).set_fill(BG, 0.55)
        dim.set_z_index(15)
        rew_t = Text("Let's go again, with details", font=SANS, font_size=40,
                     color=TEAL, weight="BOLD")
        rew_bg = RoundedRectangle(width=rew_t.width + 0.7,
                                  height=rew_t.height + 0.45,
                                  corner_radius=0.15, stroke_color=TEAL,
                                  stroke_width=2.5).set_fill(BG, 0.88)
        rew = VGroup(rew_bg.move_to(rew_t), rew_t).move_to(_p(0, 2.3))
        rew.set_z_index(20)
        self.play(FadeIn(dim), FadeIn(rew, scale=1.15),
                  FadeOut(st.res_a), FadeOut(st.res_l),
                  st.result.animate.move_to(bonsai), run_time=0.7)
        cat = make_cat(n=24).scale_to_fit_width(CAT_IN_BLOCK_W).move_to(bonsai)
        self.play(ReplacementTransform(st.result, cat), run_time=0.5)
        self.play(cat.animate.move_to(scratch), run_time=0.7)
        # rewind all the way to the capture: the cat returns to the sensor
        self.play(cat.animate.move_to(sensor), run_time=0.7)
        self.play(FadeOut(rew), FadeOut(dim), run_time=0.4)

        # replay from the capture: the image is written into memory again
        cap_r = caption("The image is captured into the global memory again.")
        self.play(FadeIn(cap_r), cat.animate.move_to(scratch),
                  Flash(p.a1.get_center(), color=TEAL, line_length=0.12),
                  run_time=0.8)
        self.wait(0.8)
        self.play(FadeOut(cap_r), run_time=0.3)

        # ---- 5: the CPU sends instructions again - how does that work? ------
        cap5 = caption("The image waits in memory. How does the CPU program BONSAI?")
        instr = Arrow(cpu.get_left() + UP * 0.4, bonsai.get_right() + UP * 0.4,
                      color=BLUE, buff=0.15, stroke_width=3.5,
                      max_tip_length_to_length_ratio=0.1)
        instr_l = Text("Instructions", font=SANS, font_size=16, color=BLUE)
        instr_l.next_to(instr, UP, buff=0.1)
        self.play(GrowArrow(instr), FadeIn(instr_l), FadeIn(cap5), run_time=0.8)
        self.wait(2.2)

        # ---- 6: zoom into the Bonsai processor ------------------------------
        cap6 = caption("Let's look inside the BONSAI Processor.")
        self.play(FadeOut(cap5), FadeIn(cap6),
                  FadeOut(VGroup(sensor, p.sensor_lbl, p.a1, p.near_die,
                                 p.near_lbl)),
                  bonsai.animate.scale(BONSAI_BIG_SCALE).move_to(BONSAI_BIG_POS),
                  p.cpu_grp.animate.move_to(CPU_EDGE),
                  VGroup(scratch, cat).animate.move_to(SCRATCH_EDGE),
                  FadeOut(p.a2), FadeOut(instr), FadeOut(instr_l),
                  run_time=2.4)

        # channels re-attach to the enlarged block (same meaning, new anchors)
        box_l = _p(bonsai.box.get_left()[0], -2.0)
        box_r = _p(bonsai.box.get_right()[0], -2.0)
        instr2 = Arrow(cpu.get_right(), box_l, color=BLUE, buff=0.15,
                       stroke_width=3.5, max_tip_length_to_length_ratio=0.14)
        instr2_l = Text("Instructions", font=SANS, font_size=15,
                        color=BLUE).next_to(instr2, UP, buff=0.08)
        data2 = Arrow(scratch.get_left(), box_r, color=GREEN, buff=0.15,
                      stroke_width=3.5, max_tip_length_to_length_ratio=0.14)
        # the cat covers the chip's own label - annotate below the block
        gm_lbl = Text("Global Memory", font=SANS, font_size=14, color=GREEN)
        gm_lbl.next_to(scratch, DOWN, buff=0.15)
        self.play(GrowArrow(instr2), FadeIn(instr2_l), GrowArrow(data2),
                  FadeIn(gm_lbl), run_time=1.1)
        self.wait(0.6)

        # ---- 7: the internals - decoder / lane / LSU / Memory Register File -
        fp = bonsai_floorplan(scale=FP_SCALE)
        fp.move_to(BONSAI_BIG_POS)
        cap7 = caption("Inside: a decoder, compute lanes, an LSU, and the Memory Register File.")
        # keep the name visible at the inner top edge of the box
        bons_lbl = Text("BONSAI Processor", font=SANS, font_size=26,
                        color=PURPLE, weight="BOLD")
        bons_lbl.move_to(bonsai.box.get_top() + DOWN * 0.38)
        self.play(FadeOut(cap6), FadeIn(cap7),
                  FadeOut(bonsai.label), FadeIn(fp), FadeIn(bons_lbl),
                  run_time=1.3)

        # instructions now target the decoder; the scratchpad feeds the LSU
        instr3 = Arrow(cpu.get_right(), fp.dec.get_left(), color=BLUE, buff=0.15,
                       stroke_width=3.5, max_tip_length_to_length_ratio=0.14)
        instr3_l = Text("Instructions", font=SANS, font_size=15,
                        color=BLUE).next_to(instr3.get_center(), UP + LEFT, buff=0.1)
        data3 = Arrow(scratch.get_left(), fp.lsu.get_right(), color=GREEN,
                      buff=0.15, stroke_width=3.5, max_tip_length_to_length_ratio=0.14)
        self.play(ReplacementTransform(instr2, instr3),
                  ReplacementTransform(instr2_l, instr3_l),
                  ReplacementTransform(data2, data3), run_time=0.7)
        self.wait(1.6)

        # ---- 8: an instruction token travels CPU -> decoder -----------------
        cap8 = caption("The CPU streams instructions into the decoder.")
        tok = token("instruction").move_to(cpu.get_center() + UP * 0.75)
        self.play(FadeOut(cap7), FadeIn(cap8), FadeIn(tok), run_time=0.5)
        self.play(tok.animate.move_to(fp.dec.get_center() + UP * 0.75), run_time=0.9)
        self.play(FadeOut(tok, target_position=fp.dec), Indicate(fp.dec, color=AMBER),
                  run_time=0.6)
        self.wait(0.5)

        # ---- 9-10: zoom into the decoder - it speaks RISC-V RVV -------------
        zoom_grp = VGroup(bonsai.box, bons_lbl, fp, instr3, instr3_l, data3,
                          p.cpu_grp, scratch, cat, gm_lbl)
        dc = fp.dec.get_center().copy()
        cap9 = caption("The decoder understands standard RISC-V vector (RVV) instructions.")
        # the "Decoder" label would fill the screen at this zoom - hide it and
        # bring it back on the way out (set_opacity, not FadeOut: FadeOut's
        # scene-removal is undone when the parent group animates afterwards)
        self.play(FadeOut(cap8), fp.dec_lbl.animate.set_opacity(0), run_time=0.3)
        self.play(zoom_grp.animate.scale(DEC_ZOOM, about_point=dc).shift(-dc),
                  run_time=1.3)

        # two-line title so it fits inside the decoder box
        title = VGroup(
            Text("RISC-V RVV", font=SANS, font_size=22, color=FG, weight="BOLD"),
            Text("vector instructions", font=SANS, font_size=22, color=FG,
                 weight="BOLD"),
        ).arrange(DOWN, buff=0.1).move_to(_p(0, 1.4))
        rows = VGroup(
            Text("vadd    v0, v1, v2", font=MONO, font_size=20, color=FG),
            Text("vmul    v3, v4, v5", font=MONO, font_size=20, color=FG),
            Text("vshift  v6, v7",     font=MONO, font_size=20, color=FG),
            Text("vgather v8, v9",     font=MONO, font_size=20, color=FG),
        ).arrange(DOWN, aligned_edge=LEFT, buff=0.34).move_to(_p(-0.55, -0.35))
        # "High programmability" badge: spawns big in the CENTRE (the claim),
        # then docks top-right while the RVV list (the evidence) reveals.
        # Echoes the "Easy programmability" card of Contribution 1.
        badge_inner = VGroup(
            Text("</>", font=MONO, font_size=26, color=GREEN),
            Text("High programmability", font=SANS, font_size=20, color=GREEN,
                 weight="BOLD"),
        ).arrange(RIGHT, buff=0.2)
        badge_box = RoundedRectangle(width=badge_inner.width + 0.5,
                                     height=badge_inner.height + 0.35,
                                     corner_radius=0.12, stroke_color=GREEN,
                                     stroke_width=2).set_fill(BG, 0.92)
        badge = VGroup(badge_box.move_to(badge_inner), badge_inner)
        badge.set_z_index(10)
        badge.move_to(ORIGIN).scale(1.35)

        self.play(FadeIn(badge, scale=1.2), run_time=0.6)
        self.wait(0.8)
        self.play(badge.animate.scale(1 / 1.35).move_to(_p(4.8, 3.4)),
                  FadeIn(title), FadeIn(cap9), run_time=0.8)
        self.play(LaggedStart(*[FadeIn(r, shift=RIGHT * 0.2) for r in rows],
                              lag_ratio=0.25), run_time=1.2)
        self.wait(1.0)

        # ---- 11-13: the compiler hovers over each line and parses it --------
        cap11 = caption("Compiled by stock LLVM and GNU compilers. No custom toolchain needed.")
        comp_rect = RoundedRectangle(width=3.8, height=0.58, corner_radius=0.1,
                                     stroke_color=GREEN, stroke_width=2).set_fill(GREEN, 0.12)
        comp_lbl = Text("LLVM / GNU compiler", font=SANS, font_size=12, color=GREEN)
        comp_lbl.move_to(comp_rect.get_right() + LEFT * (comp_lbl.width / 2 + 0.18))
        comp = VGroup(comp_rect, comp_lbl)
        comp.move_to(_p(0, rows[0].get_center()[1]))

        mcs = VGroup(*[Text(h, font=MONO, font_size=20, color=AMBER)
                       for h in ("0x0220A057", "0x02310257",
                                 "0x0A425257", "0x322C1057")])
        for r, mc in zip(rows, mcs):
            mc.move_to(r, aligned_edge=LEFT)

        self.play(FadeOut(cap9), FadeIn(cap11),
                  FadeIn(comp, shift=DOWN * 0.2), run_time=0.6)
        for i, (r, mc) in enumerate(zip(rows, mcs)):
            self.play(ReplacementTransform(r, mc), run_time=0.45)
            if i + 1 < len(rows):
                self.play(comp.animate.move_to(_p(0, rows[i + 1].get_center()[1])),
                          run_time=0.35)
        self.play(FadeOut(comp, shift=DOWN * 0.2), run_time=0.4)
        self.wait(1.0)

        # ---- 14: zoom back out to the architecture (badge fades with it) ----
        self.play(FadeOut(VGroup(title, mcs, cap11, badge)), run_time=0.5)
        self.play(zoom_grp.animate.shift(dc).scale(1.0 / DEC_ZOOM, about_point=dc),
                  run_time=1.2)
        self.play(fp.dec_lbl.animate.set_opacity(1), run_time=0.3)

        # ---- 15: the first real instruction: "Load Image" -------------------
        cap15 = caption("First instruction: load the image.")
        tok2 = token("Load Image").move_to(cpu.get_center() + UP * 0.75)
        self.play(FadeIn(cap15), FadeIn(tok2), run_time=0.5)
        self.play(tok2.animate.move_to(fp.dec.get_center() + UP * 0.75), run_time=0.9)
        self.play(FadeOut(tok2, target_position=fp.dec), Indicate(fp.dec, color=AMBER),
                  Indicate(fp.lsu, color=GREEN), run_time=0.7)

        # ---- 16: the cat streams row by row into the Memory Register File ---
        catarr = cat_image(ROWS)
        plane = fp.rf.top
        cell = plane.width / ROWS
        rf_c = plane.get_center()
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
            row.arrange(RIGHT, buff=0)
            row.move_to([cat_x, cat_top - (r + 0.5) * buf_step, 0.0])
            row.set_opacity(0.0)
            return row

        def target(r):
            return np.array([rf_c[0], rf_c[1] + ((ROWS - 1) / 2 - r) * cell, 0.0])

        # progressive caption: each segment appears as the FIRST section of
        # the image reaches that component
        segs = VGroup(
            Text("Section by section:", font=SANS, font_size=24, color=FG),
            Text("global memory", font=SANS, font_size=24, color=GREEN),
            Text("->  LSU", font=SANS, font_size=24, color=FG),
            Text("->  compute lanes", font=SANS, font_size=24, color=FG),
            Text("->  Memory Register File", font=SANS, font_size=24, color=FG),
        ).arrange(RIGHT, buff=0.25)
        if segs.width > 12.8:
            segs.scale_to_fit_width(12.8)
        segs.to_edge(DOWN, buff=0.5)

        # the first section travels SLOWLY, one component at a time
        row0 = make_row(0)
        self.add(row0)
        self.play(FadeOut(cap15), FadeIn(segs[0]),
                  row0.animate(run_time=0.4).set_opacity(1.0))
        self.play(FadeIn(segs[1]), run_time=0.5)
        self.play(row0.animate.move_to(lsu_c), FadeIn(segs[2]), run_time=1.0)
        self.play(row0.animate.move_to(lanes_c), FadeIn(segs[3]), run_time=1.0)
        self.play(row0.animate.move_to(target(0)), FadeIn(segs[4]), run_time=1.0)

        # ...then the rest streams at full pace
        load_rows = [row0]
        anims = []
        for r in range(1, ROWS):
            row = make_row(r)
            load_rows.append(row)
            path = VMobject().set_points_as_corners(
                [row.get_center(), lsu_c, lanes_c, target(r)])
            anims.append(Succession(
                row.animate(run_time=0.12).set_opacity(1.0),
                MoveAlongPath(row, path, run_time=1.2),
            ))
        self.add(*load_rows[1:])
        self.play(LaggedStart(*anims, lag_ratio=0.07), run_time=5.0)
        self.play(Indicate(fp.rf, color=TEAL, scale_factor=1.05), run_time=0.5)
        self.wait(1.0)

        # ---- swap the assembled rows for one carried cat grid ---------------
        # (same look on screen; gives Scene 7 a single object to re-add)
        rf_cat = make_cat(n=24)
        rf_cat.scale_to_fit_width(plane.width).move_to(plane.get_center())
        self.add(rf_cat)
        self.remove(*load_rows)
        self.play(FadeOut(segs), run_time=0.5)
        self.wait(0.3)
        # NOTE: no fadeout - Scene 7 re-adds this exact state (matched cut)
