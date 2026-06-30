"""Shared building blocks: labelled boxes and arrows for the die diagrams."""
from manim import RoundedRectangle, Text, VGroup, Arrow, ManimColor
from .palette import FG, SANS


def labeled_box(label, w, h, color, fill_opacity=0.16, font_size=20,
                font=SANS, text_color=None, line_spacing=0.8):
    """A rounded, colour-coded box with a centred label. Returns a VGroup with
    `.box` and `.label` attributes for convenient anchoring."""
    box = RoundedRectangle(
        width=w, height=h, corner_radius=0.08,
        stroke_color=color, stroke_width=2.0,
    ).set_fill(color, opacity=fill_opacity)
    t = Text(label, font=font, font_size=font_size,
             color=text_color or FG, line_spacing=line_spacing)
    t.move_to(box)
    if t.width > w * 0.9:
        t.scale_to_fit_width(w * 0.9)
    g = VGroup(box, t)
    g.box = box
    g.label = t
    g.accent = color
    return g


def connect(a, b, color, label=None, stroke_width=3, label_size=16,
            buff=0.12, both=False):
    """Arrow from box a to box b (edge to edge along x or y)."""
    arr = Arrow(
        a.get_center(), b.get_center(), color=color, buff=buff,
        stroke_width=stroke_width, max_tip_length_to_length_ratio=0.08,
    )
    # snap endpoints to the nearest edges for a cleaner look
    arr.put_start_and_end_on(
        a.get_boundary_point(b.get_center() - a.get_center()),
        b.get_boundary_point(a.get_center() - b.get_center()),
    )
    grp = VGroup(arr)
    grp.arrow = arr
    if label:
        lbl = Text(label, font=SANS, font_size=label_size, color=color)
        lbl.next_to(arr, _perp_dir(arr), buff=0.08)
        grp.add(lbl)
        grp.text = lbl
    return grp


def _perp_dir(arrow):
    from manim import UP, RIGHT
    v = arrow.get_end() - arrow.get_start()
    return UP if abs(v[0]) >= abs(v[1]) else RIGHT
