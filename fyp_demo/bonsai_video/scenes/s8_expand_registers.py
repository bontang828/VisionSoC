"""Scene 8 - Zoom into the register file. The SAME simplified stack (32 plain
planes) moves to centre and its planes distribute into 32 individual square
registers, named v0..v31 in place. No second/complex model is used."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from manim import (
    Scene, VGroup, Text, Rectangle, DashedLine, FadeIn, FadeOut, LaggedStart,
    ReplacementTransform, AnimationGroup, ORIGIN, UP, DOWN,
)
from common.arch import make_reg_file, register_array
from common.layout import RF_POS, RF_SIZE
from common.palette import FG, TEAL, BLUE, MONO, SANS, caption

ARRAY_SHIFT = UP * 0.35
ARRAY_SIZE = 0.95


def distributed_targets():
    arr = register_array(rows=4, cols=8, size=ARRAY_SIZE, grid=False, color=BLUE).shift(ARRAY_SHIFT)
    return [p.get_center() for p in arr.planes]


class ExpandRegisters(Scene):
    def construct(self):
        stack = make_reg_file(size=RF_SIZE).move_to(RF_POS)   # carried in from Scene 5
        self.add(stack)
        cap0 = caption("Zoom into the register file.")
        self.play(FadeIn(cap0), run_time=0.5)
        self.wait(1.0)

        # cap0 stays through the distribution; it leaves when cap arrives
        self.play(stack.animate.move_to(ORIGIN + ARRAY_SHIFT), run_time=0.7)
        targets = distributed_targets()
        scale_f = ARRAY_SIZE / RF_SIZE
        planes = stack.planes
        self.play(
            LaggedStart(*[p.animate.move_to(t).scale(scale_f)
                          .set_fill(BLUE, 0.10).set_stroke(BLUE, width=2.5, opacity=1.0)
                          for p, t in zip(planes, targets)], lag_ratio=0.04),
            run_time=2.2,
        )

        labels = VGroup()
        for i, p in enumerate(planes):
            labels.add(Text(f"v{i}", font=MONO, font_size=18, color=TEAL).next_to(p, DOWN, buff=0.08))
        cap = caption("The register file is 32 two-dimensional square registers: v0 to v31.")
        self.play(LaggedStart(*[FadeIn(t) for t in labels], lag_ratio=0.03, run_time=1.6),
                  FadeOut(cap0), FadeIn(cap))
        self.wait(2.0)

        # --- register grouping: pairs stick together into ONE register -------
        def merged(a, b, name):
            """One register covering the union of a and b, named `name`.
            Dotted lines mark where the grouped registers were joined."""
            w = b.get_right()[0] - a.get_left()[0]
            r = Rectangle(width=w, height=a.height, stroke_color=BLUE,
                          stroke_width=2.5).set_fill(BLUE, 0.10)
            r.move_to((a.get_center() + b.get_center()) / 2)
            inner_xs = (getattr(a, "inner_xs", [])
                        + [(a.get_center()[0] + b.get_center()[0]) / 2]
                        + getattr(b, "inner_xs", []))
            dashes = VGroup(*[
                DashedLine([x, r.get_top()[1] - 0.03, 0],
                           [x, r.get_bottom()[1] + 0.03, 0],
                           dash_length=0.07,
                           stroke_width=1.6).set_stroke(BLUE, opacity=0.6)
                for x in inner_xs
            ])
            g = VGroup(r, dashes)
            g.inner_xs = inner_xs
            lbl = Text(name, font=MONO, font_size=18,
                       color=TEAL).next_to(r, DOWN, buff=0.08)
            return g, lbl

        cap_g1 = caption("Registers can be grouped: each pair acts as ONE wider register (v0 + v1 -> v0).")
        pair_rects, pair_labels, merges1 = [], [], []
        for k in range(16):
            i = 2 * k
            r, lbl = merged(planes[i], planes[i + 1], f"v{i}")
            pair_rects.append(r)
            pair_labels.append(lbl)
            merges1.append(AnimationGroup(
                ReplacementTransform(VGroup(planes[i], planes[i + 1]), r),
                ReplacementTransform(VGroup(labels[i], labels[i + 1]), lbl)))
        self.play(FadeOut(cap), FadeIn(cap_g1),
                  LaggedStart(*merges1, lag_ratio=0.04), run_time=1.8)
        self.wait(2.0)

        # --- one more level: two pairs group into one (v0..v3 -> v0) ---------
        cap_g2 = caption("Group pairs again: v0 to v3 act as one, fitting larger images.")
        quad_rects, quad_labels, merges2 = [], [], []
        for j in range(8):
            r, lbl = merged(pair_rects[2 * j], pair_rects[2 * j + 1], f"v{4 * j}")
            quad_rects.append(r)
            quad_labels.append(lbl)
            merges2.append(AnimationGroup(
                ReplacementTransform(VGroup(pair_rects[2 * j], pair_rects[2 * j + 1]), r),
                ReplacementTransform(VGroup(pair_labels[2 * j], pair_labels[2 * j + 1]), lbl)))
        self.play(FadeOut(cap_g1), FadeIn(cap_g2),
                  LaggedStart(*merges2, lag_ratio=0.06), run_time=1.6)
        self.wait(1.8)

        # --- ungroup: back to the 32 registers (the state Scene 10 carries in)
        cap_g3 = caption("Ungrouped again: 32 independent registers.")
        arr2 = register_array(rows=4, cols=8, size=ARRAY_SIZE, grid=False,
                              color=BLUE).shift(ARRAY_SHIFT)
        labels2 = VGroup(*[Text(f"v{i}", font=MONO, font_size=18, color=TEAL)
                           .next_to(p, DOWN, buff=0.08)
                           for i, p in enumerate(arr2.planes)])
        splits = []
        for j in range(8):
            splits.append(AnimationGroup(
                ReplacementTransform(quad_rects[j],
                                     VGroup(*arr2.planes[4 * j:4 * j + 4])),
                ReplacementTransform(quad_labels[j],
                                     VGroup(*labels2[4 * j:4 * j + 4]))))
        self.play(FadeOut(cap_g2), FadeIn(cap_g3),
                  LaggedStart(*splits, lag_ratio=0.05), run_time=1.4)
        self.wait(1.0)
        self.play(FadeOut(cap_g3), run_time=0.5)
        self.wait(0.2)
