"""Reusable PixelGrid mobject for the BONSAI demo.

A PixelGrid is a VGroup of Squares laid out in row-major order. It maps an
n x m array of values in [0,1] to grayscale cells, with helpers for indexing,
highlighting, and recolouring (for the per-instruction sweep effect).
"""
import numpy as np
from manim import (
    VGroup, Square, Integer, rgb_to_color, WHITE, BLACK,
)
from .palette import TEAL


class PixelGrid(VGroup):
    def __init__(
        self,
        values,
        cell_size=0.4,
        stroke_opacity=0.22,
        stroke_width=1.0,
        show_values=False,
        value_scale=0.45,
        **kwargs,
    ):
        super().__init__(**kwargs)
        self.values = np.asarray(values, dtype=float)
        self.is_color = self.values.ndim == 3
        if self.is_color:
            self.rows, self.cols = self.values.shape[:2]
        else:
            self.rows, self.cols = self.values.shape
        self.cell_size = cell_size

        cells = VGroup()
        self.cell_list = []
        for r in range(self.rows):
            for c in range(self.cols):
                if self.is_color:
                    fill = rgb_to_color(self.values[r, c])
                else:
                    v = float(self.values[r, c])
                    fill = rgb_to_color([v, v, v])
                sq = Square(side_length=cell_size)
                sq.set_fill(fill, opacity=1.0)
                sq.set_stroke(WHITE, width=stroke_width, opacity=stroke_opacity)
                cells.add(sq)
                self.cell_list.append(sq)
        cells.arrange_in_grid(rows=self.rows, cols=self.cols, buff=0)
        self.cells = cells
        self.add(cells)

        self.value_labels = VGroup()
        if show_values:
            for idx, sq in enumerate(self.cell_list):
                val = int(round(self.values.flat[idx] * 255))
                # dark text on bright cells, light text on dark cells
                col = BLACK if self.values.flat[idx] > 0.5 else WHITE
                lbl = Integer(val, color=col).scale(value_scale).move_to(sq)
                self.value_labels.add(lbl)
            self.add(self.value_labels)

    # --- indexing -----------------------------------------------------------
    def cell(self, r, c):
        return self.cell_list[r * self.cols + c]

    def cell_center(self, r, c):
        return self.cell(r, c).get_center()

    # --- styling helpers ----------------------------------------------------
    def recolor_from(self, values):
        """Return a list of (cell, target_color) for animating a value change."""
        values = np.asarray(values, dtype=float)
        out = []
        for idx, sq in enumerate(self.cell_list):
            v = float(values.flat[idx])
            out.append((sq, rgb_to_color([v, v, v])))
        return out

    def outline(self, color=TEAL, width=4.0):
        """A surrounding rectangle matching the grid extent."""
        from manim import SurroundingRectangle
        return SurroundingRectangle(self.cells, color=color, buff=0.0, stroke_width=width)
