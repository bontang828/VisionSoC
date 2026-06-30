"""Simple vector icons for the BONSAI demo: bonsai tree, camera sensor,
lightning bolt, and a generic chip/die block."""
import numpy as np
from manim import (
    VGroup, Polygon, Rectangle, RoundedRectangle, Circle, Line, Dot, Text,
    ManimColor, WHITE, BLACK, UP, DOWN, LEFT, RIGHT,
)
from .palette import FG, MUTED, TEAL, AMBER, GREEN, BLUE

BROWN = ManimColor("#8d6e63")
LEAF = ManimColor("#5fa85f")
LEAF_D = ManimColor("#3f7d44")


def _p(x, y):
    return np.array([x, y, 0.0])


def bonsai_tree(scale=1.0):
    """A small stylised bonsai: pot, curved trunk, three foliage clusters."""
    pot = Polygon(_p(-0.6, -1.0), _p(0.6, -1.0), _p(0.46, -0.62), _p(-0.46, -0.62),
                  color=BROWN, fill_opacity=1.0, stroke_width=0)
    pot_rim = Rectangle(width=1.28, height=0.14, color=BROWN, fill_opacity=1.0,
                        stroke_width=0).move_to(_p(0, -0.55))
    trunk = Polygon(_p(-0.08, -0.55), _p(0.08, -0.55), _p(0.18, 0.15), _p(0.02, 0.15),
                    color=BROWN, fill_opacity=1.0, stroke_width=0)
    f1 = Circle(radius=0.5, color=LEAF, fill_opacity=1.0, stroke_width=0).move_to(_p(0.1, 0.55))
    f2 = Circle(radius=0.38, color=LEAF_D, fill_opacity=1.0, stroke_width=0).move_to(_p(-0.42, 0.35))
    f3 = Circle(radius=0.36, color=LEAF, fill_opacity=1.0, stroke_width=0).move_to(_p(0.55, 0.3))
    tree = VGroup(pot, pot_rim, trunk, f2, f3, f1)
    tree.scale(scale)
    return tree


def camera_sensor(size=1.4, color=BLUE):
    """A camera/image-sensor icon: chip body + lens."""
    body = RoundedRectangle(width=size, height=size, corner_radius=0.1,
                            stroke_color=color, stroke_width=2.5)
    body.set_fill(color, 0.12)
    lens_o = Circle(radius=size * 0.32, color=color, stroke_width=2.5).move_to(body)
    lens_i = Circle(radius=size * 0.16, color=TEAL, stroke_width=2).set_fill(TEAL, 0.25).move_to(body)
    glint = Dot(point=body.get_center() + _p(size * 0.09, size * 0.09), radius=0.03, color=WHITE)
    g = VGroup(body, lens_o, lens_i, glint)
    g.body = body
    return g


def lightning(scale=1.0, color=AMBER):
    pts = [_p(0.15, 0.5), _p(-0.18, 0.05), _p(0.02, 0.05), _p(-0.15, -0.5),
           _p(0.22, 0.02), _p(0.02, 0.02)]
    bolt = Polygon(*pts, color=color, fill_opacity=1.0, stroke_width=0)
    bolt.scale(scale)
    return bolt


def chip(label, w=1.8, h=1.2, color=MUTED, font_size=22, fill_opacity=0.10):
    box = RoundedRectangle(width=w, height=h, corner_radius=0.1,
                           stroke_color=color, stroke_width=2.5).set_fill(color, fill_opacity)
    t = Text(label, font_size=font_size, color=FG).move_to(box)
    if t.width > w * 0.85:
        t.scale_to_fit_width(w * 0.85)
    g = VGroup(box, t)
    g.box = box
    g.label = t
    return g
