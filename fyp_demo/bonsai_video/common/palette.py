"""Shared colours, fonts and small text helpers for the BONSAI demo video.

Keep every scene importing these so the look stays consistent.
"""
from manim import Text, ManimColor, DOWN, UP

from . import pacing  # noqa: F401  (side effect: global 1.2x slowdown)

# --- Core palette -----------------------------------------------------------
BG = ManimColor("#0e0e12")        # near-black background (also set in manim.cfg)
FG = ManimColor("#e6e6ea")        # primary text / foreground
MUTED = ManimColor("#8a8a99")     # secondary text / captions
TEAL = ManimColor("#2dd4bf")      # active sweep / highlight (the "live" colour)
AMBER = ManimColor("#f5a623")     # results / output accent
BLUE = ManimColor("#6c8ebf")      # code keywords / data path
MAGENTA = ManimColor("#c77dff")   # transpose / vertical-mode accent
GREEN = ManimColor("#82b366")     # on-die / sensor path (matches die diagram)
PURPLE = ManimColor("#b39ddb")    # 2D vector fabric (matches die diagram)
ORANGE = ManimColor("#f0a868")    # DMA / data movement (matches die diagram)
RED = ManimColor("#e07a5f")       # high-bandwidth AXI (matches die diagram)

# --- Typography -------------------------------------------------------------
# Consolas ships on Windows; fall back is handled by Manim/Pango if absent.
MONO = "Consolas"
SANS = "Segoe UI"

TITLE_SIZE = 54
CAPTION_SIZE = 30
CODE_SIZE = 30
LABEL_SIZE = 24


def caption(text, color=FG, size=CAPTION_SIZE, max_width=12.8):
    """A single-line caption pinned to the lower third, auto-fit to frame width."""
    t = Text(text, font=SANS, font_size=size, color=color)
    if t.width > max_width:
        t.scale_to_fit_width(max_width)
    t.to_edge(DOWN, buff=0.5)
    return t


def title_text(text, color=FG, size=TITLE_SIZE):
    return Text(text, font=SANS, font_size=size, color=color, weight="BOLD")


def code_line(text, color=FG, size=CODE_SIZE):
    """Monospace code line. Use t2c on the result for keyword colouring."""
    return Text(text, font=MONO, font_size=size, color=color)
