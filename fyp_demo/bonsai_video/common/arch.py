"""Architecture mobjects for the BONSAI demo (simplified):
stacked square register planes, a single register plane, and the Bonsai
floor-plan (register file + compute lanes + LSU + decoder)."""
import numpy as np
from manim import (
    VGroup, Square, Rectangle, RoundedRectangle, Line, Text, Arrow,
    DoubleArrow, UP, DOWN, LEFT, RIGHT,
)
from .palette import FG, MUTED, TEAL, AMBER, BLUE, PURPLE, GREEN, ORANGE


def _p(x, y):
    return np.array([x, y, 0.0])


def grid_lines(square, n=8, color=BLUE, opacity=0.35, width=1.0):
    """Faint n x n grid lines covering a Square."""
    g = VGroup()
    c = square.get_center()
    s = square.side_length
    for i in range(1, n):
        f = -s / 2 + s * i / n
        g.add(Line(c + _p(f, -s / 2), c + _p(f, s / 2), stroke_width=width).set_stroke(color, opacity=opacity))
        g.add(Line(c + _p(-s / 2, f), c + _p(s / 2, f), stroke_width=width).set_stroke(color, opacity=opacity))
    return g


def register_plane(size=2.2, color=BLUE, n=8, fill_opacity=0.10, grid=True):
    """A single 2D square register plane with a faint grid."""
    sq = Square(size, stroke_color=color, stroke_width=2.5).set_fill(color, fill_opacity)
    g = VGroup(sq)
    g.square = sq
    if grid:
        g.add(grid_lines(sq, n=n, color=color))
    return g


def register_stack(n_planes=6, size=2.2, dx=0.16, dy=0.13, color=BLUE, grid=True):
    """An isometric stack of square register planes (back-to-front, up-right).
    Returns a VGroup with `.top` set to the front-most plane."""
    g = VGroup()
    planes = []
    for i in range(n_planes):
        op = 0.06 + 0.10 * (i / max(1, n_planes - 1))
        sq = Square(size, stroke_color=color, stroke_width=2.0).set_fill(color, op)
        sq.set_stroke(opacity=0.4 + 0.5 * (i / max(1, n_planes - 1)))
        sq.shift(_p(dx * i, dy * i))
        planes.append(sq)
        g.add(sq)
    top = planes[-1]
    if grid:
        g.add(grid_lines(top, n=8, color=color, opacity=0.4))
    g.top = top
    g.planes = planes
    return g


def register_array(rows=4, cols=8, size=1.0, gap=0.55, color=BLUE, n_grid=6, grid=True):
    """A grid of individual square register planes (rows*cols of them).
    Returns a VGroup with `.planes` (row-major list)."""
    g = VGroup()
    planes = []
    pitch_x = size + gap
    pitch_y = size + gap + 0.25  # extra room for a name label under each
    x0 = -(cols - 1) * pitch_x / 2
    y0 = (rows - 1) * pitch_y / 2
    for r in range(rows):
        for c in range(cols):
            p = register_plane(size=size, color=color, n=n_grid, grid=grid)
            p.move_to(_p(x0 + c * pitch_x, y0 - r * pitch_y))
            planes.append(p)
            g.add(p)
    g.planes = planes
    return g


# The register file is ONE simplified model used everywhere (floor-plan, expand,
# collapse, scale): a clean stack of plain planes that can distribute 1:1 to 32.
RF_PLANES = 32


RF_DX, RF_DY = 0.022, 0.018


def rf_plane_opacity(i, n=None):
    """Depth gradient: back planes faint, front plane prominent. Returns
    (stroke_opacity, fill_opacity)."""
    n = n or RF_PLANES
    t = i / (n - 1)
    return 0.08 + 0.92 * (t ** 1.6), 0.03 + 0.10 * t


def make_reg_file(size=2.0, color=BLUE):
    """The simplified register-file stack: RF_PLANES planes offset into a deck
    with a depth gradient (one model, used in Scenes 5/8/10/11)."""
    g = VGroup()
    planes = []
    for i in range(RF_PLANES):
        so, fo = rf_plane_opacity(i)
        sq = Square(size, stroke_color=color, stroke_width=2.5).set_fill(color, fo)
        sq.set_stroke(opacity=so)
        sq.shift(_p(RF_DX * i, RF_DY * i))
        planes.append(sq)
        g.add(sq)
    g.move_to(_p(0, 0))
    g.planes = planes
    g.top = planes[-1]
    return g


def compute_lane(n=8, color=GREEN, cw=0.45, ch=0.6, gap=0.12):
    """A single compute lane: a green box holding n lane cells."""
    cells = VGroup(*[Rectangle(width=cw, height=ch, stroke_color=color, stroke_width=2).set_fill(color, 0.12)
                     for _ in range(n)])
    cells.arrange(RIGHT, buff=gap)
    box = RoundedRectangle(width=cells.width + 0.5, height=ch + 0.4, corner_radius=0.08,
                           stroke_color=color, stroke_width=2).set_fill(color, 0.05).move_to(cells)
    g = VGroup(box, cells)
    g.box = box
    g.cells = cells
    return g


def bonsai_floorplan(scale=1.0):
    """Simplified Bonsai floor-plan: register-file stack on top, compute lanes
    below, an LSU to the side, and a decoder feeding instructions in.
    Returns a VGroup with named attributes for anchoring/animation."""
    g = VGroup()

    # register file (the one simplified stack model used everywhere)
    rf = make_reg_file(size=2.0)
    rf.move_to(_p(0, 1.0))
    # "Memory Register File" so viewers who don't know registers still get it
    rf_lbl = Text("Memory Register File", font_size=20, color=BLUE).next_to(rf, UP, buff=0.35)

    # compute lanes (a band of lane cells) directly below
    lanes = VGroup()
    n_lanes = 8
    for i in range(n_lanes):
        cell = Rectangle(width=0.42, height=0.6, stroke_color=GREEN, stroke_width=2).set_fill(GREEN, 0.12)
        lanes.add(cell)
    lanes.arrange(RIGHT, buff=0.12).move_to(_p(0, -1.1))
    lanes_box = RoundedRectangle(width=lanes.width + 0.5, height=1.0, corner_radius=0.08,
                                 stroke_color=GREEN, stroke_width=2).set_fill(GREEN, 0.05).move_to(lanes)
    lanes_lbl = Text("Compute Lanes", font_size=20, color=GREEN).next_to(lanes_box, DOWN, buff=0.2)

    # LSU to the right
    lsu = RoundedRectangle(width=1.0, height=1.0, corner_radius=0.08,
                           stroke_color=PURPLE, stroke_width=2.5).set_fill(PURPLE, 0.12)
    lsu.next_to(lanes_box, RIGHT, buff=0.7)
    lsu_lbl = Text("LSU", font_size=20, color=PURPLE).move_to(lsu)

    # decoder to the left
    dec = RoundedRectangle(width=1.0, height=1.0, corner_radius=0.08,
                           stroke_color=AMBER, stroke_width=2.5).set_fill(AMBER, 0.12)
    dec.next_to(lanes_box, LEFT, buff=0.7)
    dec_lbl = Text("Decoder", font_size=18, color=AMBER).move_to(dec)

    # arrows: register file <-> lanes ; lanes <-> lsu ; decoder -> lanes
    a_rf = Arrow(rf.get_bottom(), lanes_box.get_top(), color=BLUE, buff=0.1,
                 stroke_width=3, max_tip_length_to_length_ratio=0.12)
    # double-headed: data flows both ways between the lanes and the LSU
    a_lsu = DoubleArrow(lanes_box.get_right(), lsu.get_left(), color=PURPLE,
                        buff=0.08, stroke_width=3,
                        max_tip_length_to_length_ratio=0.18)
    a_dec = Arrow(dec.get_right(), lanes_box.get_left(), color=AMBER, buff=0.08,
                  stroke_width=3, max_tip_length_to_length_ratio=0.18)

    g.add(rf, rf_lbl, lanes_box, lanes, lanes_lbl, lsu, lsu_lbl, dec, dec_lbl,
          a_rf, a_lsu, a_dec)
    g.rf = rf
    g.rf_lbl = rf_lbl
    g.lanes = lanes
    g.lanes_box = lanes_box
    g.lanes_lbl = lanes_lbl
    g.lsu = lsu
    g.lsu_lbl = lsu_lbl
    g.dec = dec
    g.dec_lbl = dec_lbl
    g.arrows = VGroup(a_rf, a_lsu, a_dec)
    g.scale(scale)
    return g
