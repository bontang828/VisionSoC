"""Canonical positions / sizes shared across scenes so that elements can be
carried from one scene to the next (a scene ends with an element exactly where
the next scene re-adds it, giving a seamless cut)."""
import numpy as np
from manim import ImageMobject
from common.assets import cat_image, _cat_file, generate_popcat
from common.pixel_grid import PixelGrid

# --- the cat (the subject) -------------------------------------------------
CAT_N = 32
CAT_CELL = 0.12                        # consistent size everywhere (32*0.12 = 3.84)
CAT_CENTER = np.array([0.0, 0.35, 0.0])
CAT_LEFT = np.array([-4.5, 0.35, 0.0])
# small downscaled cat used in the three-pipeline comparison (Scene 3.5)
CAT_SMALL_POS = np.array([-6.3, 0.0, 0.0])
CAT_SMALL_W = 1.05


def make_cat_photo(width=1.0):
    """A flat photo of the cat (ImageMobject) - fast to render. Used for the
    input image in Scenes 2 and 3.5 (the register scenes keep the pixel grid)."""
    f = _cat_file()
    if f:
        im = ImageMobject(f)
    else:
        im = ImageMobject((np.clip(generate_popcat(64), 0, 1) * 255).astype("uint8"))
    im.scale_to_fit_width(width)
    return im


def make_cat(cell=CAT_CELL, n=CAT_N):
    return PixelGrid(cat_image(n), cell_size=cell)


def make_buffer_cat(width=0.8):
    """A small cat sitting inside the buffer (used to carry 4 -> 5)."""
    c = make_cat()
    c.scale_to_fit_width(width)
    c.move_to(BUFFER_POS)
    return c


# --- near-sensor die inner blocks (scenes 4 & 5) ---------------------------
# Lowered so the conventional pipeline (top) and near-sensor pipeline (bottom)
# can share the screen in the Scene 3/4 comparison.
SENSOR_POS = np.array([0.7, -0.7, 0.0])
BUFFER_POS = np.array([2.4, -0.7, 0.0])
BONSAI_POS = np.array([4.2, -0.7, 0.0])

# --- floor-plan / register file (scenes 5, 8, 10, 11) ----------------------
FLOORPLAN_CENTER = np.array([0.0, -0.2, 0.0])
RF_POS = np.array([0.0, 0.8, 0.0])     # register-file stack centre inside floor-plan
RF_SIZE = 2.0
