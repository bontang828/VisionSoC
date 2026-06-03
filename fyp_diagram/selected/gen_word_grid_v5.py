#!/usr/bin/env python3
"""Build v5 from a fresh copy of v4 (run: cp v4 -> v5, then this script).

Main grid is simplified: each word cell shows only `word N` / `bank B`
(N = per-bank word index = 2*row + (col>=8), B = (row+col) mod 8) in the
bank colour -- no byte sub-grid.

A single DETAILED row (v4 style: each word = 4x2 grid of its 8 byte indices)
is added at the bottom, mirroring Row 0 but ~15% larger and centred so it is
wider than Row 0; two dotted callout lines therefore run diagonally from
Row 0 down to it. Detail cells are labelled `word N bank B` + byte range.
"""
import xml.etree.ElementTree as ET

PATH = "vrf_diagonal_banking_v5_per_word.drawio"

BANK = ["#F2C9C9", "#F6D7B0", "#F3E9B5", "#CFE3C0",
        "#B8E0D4", "#BBD7EC", "#CBC2E0", "#EBC6DC"]
GREY = "light-dark(#cccccc, #543131)"

CW, CH = 71.75, 70.75
GX, GW = 968.3464864864864, 73.6891891891892
XCOL = lambda c: 140.5 + c * CW
ROWY = {0: 183.0, 1: 253.75, 2: 324.5, 3: 395.25}
DOTSY = 466.0
R127Y = 490.0

tree = ET.parse(PATH)
mxfile = tree.getroot()
model = mxfile.find('.//mxGraphModel')
root = model.find('root')

model.set('pageHeight', '950')

# ---- drop the v4-generated cells (regenerate fresh for v5) ---------------
def is_gen(cid):
    return cid and cid.split('_')[0] in ("wc", "bt", "dots", "colh", "io")

for cell_el in list(root):
    if is_gen(cell_el.get('id')):
        root.remove(cell_el)

# ---- helpers ------------------------------------------------------------
def cell(cid, value, style, x, y, w, h):
    c = ET.SubElement(root, 'mxCell')
    c.set('id', cid); c.set('value', value); c.set('style', style)
    c.set('parent', '1'); c.set('vertex', '1')
    g = ET.SubElement(c, 'mxGeometry')
    g.set('x', str(round(x, 3))); g.set('y', str(round(y, 3)))
    g.set('width', str(round(w, 3))); g.set('height', str(round(h, 3)))
    g.set('as', 'geometry')
    return c

def dline(cid, x1, y1, x2, y2):
    c = ET.SubElement(root, 'mxCell')
    c.set('id', cid); c.set('value', '')
    c.set('style', "endArrow=none;dashed=1;html=1;strokeColor=#1565C0;"
                   "strokeWidth=2;dashPattern=6 6;")
    c.set('parent', '1'); c.set('edge', '1')
    g = ET.SubElement(c, 'mxGeometry'); g.set('relative', '1'); g.set('as', 'geometry')
    s = ET.SubElement(g, 'mxPoint'); s.set('x', str(x1)); s.set('y', str(y1)); s.set('as', 'sourcePoint')
    t = ET.SubElement(g, 'mxPoint'); t.set('x', str(x2)); t.set('y', str(y2)); t.set('as', 'targetPoint')

SIMPLE_STYLE = ("rounded=0;whiteSpace=wrap;html=1;fillColor={fill};strokeColor=#FFFFFF;"
                "strokeWidth=2;fontSize=12;fontColor=#212121;align=center;verticalAlign=middle;"
                "movable=1;resizable=1;rotatable=1;deletable=1;editable=1;locked=0;connectable=1;")
WORD_STYLE = ("rounded=0;whiteSpace=wrap;html=1;fillColor={fill};strokeColor=#FFFFFF;"
              "strokeWidth=2;fontSize=11;fontColor=#212121;align=center;verticalAlign=top;"
              "movable=1;resizable=1;rotatable=1;deletable=1;editable=1;locked=0;connectable=1;")
BYTE_STYLE = ("rounded=0;whiteSpace=wrap;html=1;fillColor={fill};strokeColor=#FFFFFF;"
              "strokeWidth=1;fontSize=10;fontColor=#212121;align=center;verticalAlign=middle;"
              "movable=1;resizable=1;rotatable=1;deletable=1;editable=1;locked=0;connectable=1;")
LABEL_FRAC = 32.0 / CH   # label band as a fraction of cell height

def simple_word(rlabel, c, X, Y, fill, n, b, W=CW):
    cell("sc_{}_{}".format(rlabel, c), "word {}\nbank {}".format(n, b),
         SIMPLE_STYLE.format(fill=fill), X, Y, W, CH)

def detail_word(cid, label, X, Y, W, H, fill, lo):
    cell(cid, label, WORD_STYLE.format(fill=fill), X, Y, W, H)
    bw = W / 4.0
    lh = H * LABEL_FRAC
    bh = (H - lh) / 2.0
    for i in range(8):
        col, sub = i % 4, i // 4
        cell("{}_{}".format(cid, i), str(lo + i), BYTE_STYLE.format(fill=fill),
             X + col * bw, Y + lh + sub * bh, bw, bh)

# ---- simplified grid: rows 0-3 cols 0-10 + grey word-15 -----------------
for r in range(4):
    for c in range(11):
        b = (r + c) % 8
        n = 2 * r + (1 if c >= 8 else 0)
        simple_word(str(r), c, XCOL(c), ROWY[r], BANK[b], n, b)
    simple_word(str(r), 15, GX, ROWY[r], GREY, 2 * r + 1, (r + 15) % 8, W=GW)

# ---- "..." skipped-rows row ---------------------------------------------
DOT_STYLE = ("rounded=0;whiteSpace=wrap;html=1;fillColor=light-dark(#cccccc, #543131);"
             "strokeColor=#FFFFFF;strokeWidth=2;fontSize=12;fontColor=#212121;align=center;"
             "verticalAlign=middle;movable=1;resizable=1;rotatable=1;deletable=1;editable=1;"
             "locked=0;connectable=1;")
for c in range(11):
    cell("dots_{}".format(c), "...", DOT_STYLE, XCOL(c), DOTSY, CW, 25.0)

# ---- row 127 (greyed) ---------------------------------------------------
for c in range(11):
    b = (127 + c) % 8
    n = 254 + (1 if c >= 8 else 0)
    simple_word("127", c, XCOL(c), R127Y, GREY, n, b)
simple_word("127", 15, GX, R127Y, GREY, 255, (127 + 15) % 8, W=GW)

# ---- column headers + top I/O -------------------------------------------
HDR_STYLE = ("text;html=1;align=center;verticalAlign=middle;fontSize=12;fontStyle=1;"
             "fontColor=#37474F;movable=1;resizable=1;rotatable=1;deletable=1;editable=1;"
             "locked=0;connectable=1;")
for c in range(11):
    cell("colh_{}".format(c), "col {}".format(c), HDR_STYLE, XCOL(c), 102.0, CW, 22.0)
cell("colh_dots", "...", HDR_STYLE, 932.5, 102.0, 35.27, 22.0)
cell("colh_15", "col 15", HDR_STYLE, GX, 102.0, GW, 22.0)

IO_STYLE = ("shape=trapezoid;perimeter=trapezoidPerimeter;whiteSpace=wrap;html=1;fixedSize=1;"
            "size=10;movable=1;resizable=1;rotatable=1;deletable=1;editable=1;locked=0;connectable=1;")
for c in range(11):
    cell("io_{}".format(c), "I/O", IO_STYLE, XCOL(c) + 6, 160.0, 60.0, 19.43)

# ---- detailed row (mirrors Row 0), ~15% larger and centred --------------
DETY = 808.0
F = 1.15
ORIG_L, ORIG_R = 140.5, round(GX + GW, 3)          # 140.5 .. 1042.035
CENTER = (ORIG_L + ORIG_R) / 2.0
NEW_W = (ORIG_R - ORIG_L) * F
NEW_L = CENTER - NEW_W / 2.0
sx = lambda x: NEW_L + (x - ORIG_L) * F
sw = lambda w: w * F
DCH = CH * F

cell("det_head",
     "Detailed view of Row 0 — one word = 8 pixels (1 byte each), coloured by its bank",
     "text;html=1;align=left;verticalAlign=middle;fontSize=14;fontStyle=1;fontColor=#212121;"
     "movable=1;resizable=1;rotatable=1;deletable=1;editable=1;locked=0;connectable=1;",
     round(NEW_L, 3), 776.0, 760.0, 24.0)

for c in range(11):
    b = (0 + c) % 8
    n = 0 if c < 8 else 1
    lo = 8 * c
    label = "word {} bank {}\n{}-{}".format(n, b, lo, lo + 7)
    detail_word("dw_{}".format(c), label, sx(XCOL(c)), DETY, sw(CW), DCH, BANK[b], lo)

cell("dots_det", "...", DOT_STYLE, sx(932.5), DETY + (DCH - sw(25.0)) / 2.0,
     sw(35.27), sw(25.0))
detail_word("dw_15", "word 1 bank 7\n120-127", sx(GX), DETY, sw(GW), DCH, GREY, 120)

# ---- two dotted callout lines: Row 0 corners -> wider detail corners -----
y_from = ROWY[0] + CH
dline("callout_l", ORIG_L, y_from, round(NEW_L, 3), DETY)
dline("callout_r", ORIG_R, y_from, round(NEW_L + NEW_W, 3), DETY)

# ---- write back ---------------------------------------------------------
def indent(elem, level=0, space="    "):
    pad = "\n" + level * space
    if len(elem):
        if not (elem.text and elem.text.strip()):
            elem.text = pad + space
        for i, child in enumerate(elem):
            indent(child, level + 1, space)
            tail = pad + space if i < len(elem) - 1 else pad
            if not (child.tail and child.tail.strip()):
                child.tail = tail
    else:
        if level and not (elem.tail and elem.tail.strip()):
            elem.tail = pad

indent(mxfile)
tree.write(PATH, encoding="unicode")
print("done: v5 detail row enlarged (F={}), diagonal callouts, word/bank labels".format(F))
