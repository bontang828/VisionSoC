"""Image assets for the BONSAI demo.

Default subject is a friendly, COLOURED Pop-Cat. If a real image is dropped at
``bonsai_video/assets/cat.png`` it is used instead (resized, kept in colour).
"""
import os
import numpy as np

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_ASSET_DIR = os.path.join(_ROOT, "assets")
# First existing path wins (real Pop-Cat overrides the procedural one).
_CAT_CANDIDATES = [
    os.path.join(_ROOT, "popcat.jpg"),
    os.path.join(_ASSET_DIR, "popcat.jpg"),
    os.path.join(_ASSET_DIR, "cat.png"),
]


def _cat_file():
    for p in _CAT_CANDIDATES:
        if os.path.exists(p):
            return p
    return None


def _grids(n):
    coords = (np.arange(n) + 0.5) / n
    Y, X = np.meshgrid(coords, coords, indexing="ij")  # X=col (L->R), Y=row (top->bottom)
    return X, Y


def _ellipse(X, Y, cx, cy, rx, ry):
    return ((X - cx) / rx) ** 2 + ((Y - cy) / ry) ** 2 <= 1.0


def _triangle(X, Y, p1, p2, p3):
    (x1, y1), (x2, y2), (x3, y3) = p1, p2, p3
    d = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)
    a = ((y2 - y3) * (X - x3) + (x3 - x2) * (Y - y3)) / d
    b = ((y3 - y1) * (X - x3) + (x1 - x3) * (Y - y3)) / d
    c = 1 - a - b
    return (a >= 0) & (b >= 0) & (c >= 0)


def generate_popcat(n=32):
    """Return an (n, n, 3) float RGB array of a friendly coloured Pop-Cat."""
    X, Y = _grids(n)
    img = np.zeros((n, n, 3), dtype=float)

    # soft blue background gradient (top -> slightly lighter bottom)
    top = np.array([0.62, 0.82, 0.91])
    bot = np.array([0.80, 0.91, 0.96])
    for k in range(3):
        img[:, :, k] = top[k] + (bot[k] - top[k]) * Y

    def paint(mask, color):
        for k in range(3):
            img[:, :, k] = np.where(mask, color[k], img[:, :, k])

    cream = (0.97, 0.94, 0.88)
    cream_sh = (0.90, 0.86, 0.78)
    pink = (0.96, 0.72, 0.78)
    black = (0.12, 0.12, 0.14)
    maroon = (0.52, 0.16, 0.22)
    nose_pink = (0.93, 0.55, 0.62)

    # ears (cream) + inner pink
    paint(_triangle(X, Y, (0.30, 0.10), (0.19, 0.44), (0.46, 0.40)), cream)
    paint(_triangle(X, Y, (0.70, 0.10), (0.81, 0.44), (0.54, 0.40)), cream)
    paint(_triangle(X, Y, (0.31, 0.18), (0.26, 0.40), (0.42, 0.38)), pink)
    paint(_triangle(X, Y, (0.69, 0.18), (0.74, 0.40), (0.58, 0.38)), pink)

    # head (cream) with a soft lower shadow
    head = _ellipse(X, Y, 0.50, 0.58, 0.35, 0.32)
    paint(head, cream)
    paint(head & (Y > 0.74), cream_sh)

    # cheeks (blush)
    paint(_ellipse(X, Y, 0.29, 0.66, 0.06, 0.05) & head, pink)
    paint(_ellipse(X, Y, 0.71, 0.66, 0.06, 0.05) & head, pink)

    # eyes (small, friendly) + glints
    paint(_ellipse(X, Y, 0.38, 0.52, 0.045, 0.075), black)
    paint(_ellipse(X, Y, 0.62, 0.52, 0.045, 0.075), black)
    paint(_ellipse(X, Y, 0.365, 0.50, 0.014, 0.02), (1, 1, 1))
    paint(_ellipse(X, Y, 0.605, 0.50, 0.014, 0.02), (1, 1, 1))

    # nose + open mouth (the pop) with a tongue
    paint(_triangle(X, Y, (0.47, 0.61), (0.53, 0.61), (0.50, 0.655)), nose_pink)
    mouth = _ellipse(X, Y, 0.50, 0.76, 0.105, 0.12)
    paint(mouth, maroon)
    paint(mouth & (Y > 0.80), (0.85, 0.45, 0.52))  # tongue

    return np.clip(img, 0.0, 1.0)


def _load_file(path, n):
    from PIL import Image
    im = Image.open(path).convert("RGB").resize((n, n), Image.LANCZOS)
    return np.clip(np.asarray(im, dtype=float) / 255.0, 0.0, 1.0)


def cat_image(n=16):
    """The demo's main subject (coloured). Real file if present, else Pop-Cat."""
    f = _cat_file()
    if f:
        return _load_file(f, n)
    return generate_popcat(n)


def sample_image(n=16):
    return cat_image(n)
