"""Scene 3.6 - Contribution 1: the architecture.
Headline "A generalised 2D spatial near-sensor chip architecture - BONSAI",
then three feature cards reveal one by one (heterogeneous / programmable /
row & column). Part of the "Our Contributions" interlude (3.6 - 3.8) that sits
right after the three-pipeline comparison.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from manim import (
    Scene, VGroup, Square, Text, Line, RoundedRectangle, FadeIn, FadeOut,
    UP, DOWN, LEFT, RIGHT,
)
from common.palette import FG, MUTED, TEAL, AMBER, MAGENTA, BLUE, GREEN, SANS, MONO


def _p(x, y):
    return np.array([x, y, 0.0])


def kicker(step):
    """Persistent 'CONTRIBUTION n / 3' tag pinned top-left."""
    return Text(f"CONTRIBUTION  {step} / 3", font=SANS, font_size=18,
                color=TEAL).to_corner(UP + LEFT, buff=0.55)


# --- tiny feature icons -----------------------------------------------------
def icon_hetero():
    cs = VGroup(*[Square(0.22, stroke_width=0).set_fill(c, 1)
                  for c in (AMBER, TEAL, MAGENTA)])
    cs.arrange(RIGHT, buff=0.09)
    return cs


def icon_program():
    return Text("</>", font=MONO, font_size=30, color=GREEN)


def icon_rowcol():
    dots = VGroup()
    for r in range(3):
        for c in range(3):
            on_row, on_col = (r == 1), (c == 1)
            col = TEAL if (on_row or on_col) else MUTED
            op = 1.0 if (on_row or on_col) else 0.35
            dots.add(Square(0.12, stroke_width=0).set_fill(col, op).move_to(_p(c * 0.2, -r * 0.2)))
    dots.move_to(_p(0, 0))
    return dots


def feature_card(icon, title, desc, color):
    box = RoundedRectangle(width=4.0, height=2.5, corner_radius=0.15,
                           stroke_color=color, stroke_width=2).set_fill(color, 0.05)
    ic = icon.copy().move_to(box.get_top() + DOWN * 0.55)
    ttl = Text(title, font=SANS, font_size=21, color=FG, weight="BOLD")
    if ttl.width > 3.5:
        ttl.scale_to_fit_width(3.5)
    ttl.move_to(box.get_center() + UP * 0.05)
    ds = Text(desc, font=SANS, font_size=15, color=MUTED, line_spacing=0.85)
    if ds.width > 3.6:
        ds.scale_to_fit_width(3.6)
    ds.next_to(ttl, DOWN, buff=0.22)
    g = VGroup(box, ic, ttl, ds)
    return g


class ContributionArch(Scene):
    def construct(self):
        tag = kicker(1)
        head1 = Text("A generalised 2D spatial near-sensor", font=SANS,
                     font_size=32, color=FG)
        head2 = Text("chip architecture", font=SANS, font_size=32, color=FG)
        bonsai = Text("BONSAI", font=SANS, font_size=40, color=TEAL, weight="BOLD")
        head = VGroup(head1, head2).arrange(DOWN, buff=0.12)
        head.move_to(_p(0, 2.35))
        bonsai.next_to(head, DOWN, buff=0.28)

        self.play(FadeIn(tag), run_time=0.4)
        self.play(FadeIn(head, shift=UP * 0.15), run_time=0.7)
        self.play(FadeIn(bonsai, scale=1.1), run_time=0.6)

        cards = VGroup(
            feature_card(icon_hetero(), "Heterogeneous\nprocessing",
                         "Local & global operations\nacross pixel plane", AMBER),
            feature_card(icon_program(), "Easy\nprogrammability",
                         "Standard RISC-V\nvector (RVV) ISA", GREEN),
            feature_card(icon_rowcol(), "Row & column\nprocessing",
                         "Operate along rows\nand columns natively", TEAL),
        ).arrange(RIGHT, buff=0.45).move_to(_p(0, -1.55))

        for c in cards:
            self.play(FadeIn(c, shift=UP * 0.2), run_time=0.6)
        self.wait(1.6)

        self.play(FadeOut(VGroup(tag, head, bonsai, cards)), run_time=0.8)
