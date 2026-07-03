"""Scene 23 - Outro gag: the popcat (transparent GIF, mouth open/closed
frames) walks in with its mouth closed, pops it open, eats the three green
foliage circles one by one (mouth chomping on each), and leaves. The bonsai
then grows its green blobs back, each spawning from zero size."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import numpy as np
from PIL import Image as PILImage
from manim import (
    Scene, Group, Text, ImageMobject, FadeIn, FadeOut, GrowFromCenter,
    UP, DOWN, RIGHT,
)
from manim.utils.images import change_to_rgba_array
from common.icons import bonsai_tree
from common.palette import FG, SANS

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GIF = os.path.join(HERE, "pop-cat.gif")
CAT_H = 1.9
# the open mouth sits low-left of the image centre; offset the cat so the
# mouth lands on whatever it is eating
MOUTH_OFF = UP * 0.48 + RIGHT * 0.19


def _p(x, y):
    return np.array([x, y, 0.0])


def _gif_frames():
    """(open_mouth, closed_mouth) RGBA arrays from the 2-frame popcat GIF."""
    g = PILImage.open(GIF)
    g.seek(0)
    f_open = np.asarray(g.convert("RGBA"))
    g.seek(1)
    f_closed = np.asarray(g.convert("RGBA"))
    return f_open, f_closed


class Outro(Scene):
    SLOWDOWN = 1.0   # fixed-length gag: keep it snappy

    def construct(self):
        f_open, f_closed = _gif_frames()

        tree = bonsai_tree(scale=1.3).move_to(UP * 0.6)
        title = Text("BONSAI", font=SANS, font_size=56, color=FG,
                     weight="BOLD").next_to(tree, DOWN, buff=0.45)
        self.play(FadeIn(tree, scale=1.05), FadeIn(title), run_time=0.6)

        cat = ImageMobject(f_closed).scale_to_fit_height(CAT_H)
        cat.move_to(_p(8.3, 1.0))
        self.add(cat)

        def mouth(arr):
            cat.pixel_array = change_to_rgba_array(arr, cat.pixel_array_dtype)

        # walks in with the mouth closed...
        self.play(cat.animate.move_to(_p(3.6, 1.0)), run_time=0.8)
        # ...a bit further, then POP - the mouth opens
        self.play(cat.animate.move_to(_p(2.8, 1.0)), run_time=0.4)
        mouth(f_open)
        self.wait(0.3)

        # eats the foliage circles one by one, mouth chomping on each
        # (f3 right, f1 top, f2 left); regrowth copies saved beforehand
        snacks = [tree[4], tree[5], tree[3]]
        regrow = [leaf.copy() for leaf in (tree[3], tree[4], tree[5])]
        for leaf in snacks:
            tgt = leaf.get_center()
            self.play(cat.animate.move_to(tgt + MOUTH_OFF), run_time=0.45)
            self.play(FadeOut(leaf, target_position=tgt + UP * 0.1, scale=0.1),
                      run_time=0.3)
            tree.remove(leaf)     # FadeOut would flash it back in group fades
            mouth(f_closed)       # chomp!
            self.wait(0.2)
            mouth(f_open)
        mouth(f_closed)
        self.wait(0.2)

        # ...and leaves, satisfied
        self.play(cat.animate.move_to(_p(-8.6, 0.9)), run_time=1.0)

        # the bonsai grows its green blobs back, from zero to full size
        for leaf in regrow:
            tree.add(leaf)
            self.play(GrowFromCenter(leaf), run_time=0.45)
        self.wait(0.6)
        self.play(FadeOut(Group(tree, title)), run_time=0.7)
