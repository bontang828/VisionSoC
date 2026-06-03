#!/usr/bin/env python3
"""Rebuild the VRF diagonal-banking diagram so every word cell uses the
byte-grid template (label `word (r,c)` + byte range + 4x2 byte sub-grid),
coloured by bank = (row + col) mod 8.

- 11 colored word-columns (c=0..10) + grey "..." separator + grey word-15 box.
- Dual Port 1 example grid removed; Dual Port 0 header widened to span all.
"""
import xml.etree.ElementTree as ET

PATH = "vrf_diagonal_banking_v4_per_word.drawio"

BANK = ["#F2C9C9", "#F6D7B0", "#F3E9B5", "#CFE3C0",
        "#B8E0D4", "#BBD7EC", "#CBC2E0", "#EBC6DC"]
GREY = "light-dark(#cccccc, #543131)"

CW, CH = 71.75, 70.75                 # colored word cell size
GX, GW = 968.3464864864864, 73.6891891891892   # grey word-15 column
XCOL = lambda c: 140.5 + c * CW
ROWY = {0: 183.0, 1: 253.75, 2: 324.5, 3: 395.25}

tree = ET.parse(PATH)
mxfile = tree.getroot()
model = mxfile.find('.//mxGraphModel')
root = model.find('root')

# ---- IDs to drop --------------------------------------------------------
remove = set()
# Dual Port 1 / Plane 1 example grid + its labels / dims
remove |= {str(i) for i in range(303, 315)}          # 303-314 colored example
remove |= {"321", "322", "323", "318", "319", "320"} # row127 + "..." example
remove |= {"315", "316", "317", "325", "337", "326", "327"}  # IO/headers/dims
# regenerated regions
remove |= {str(i) for i in range(157, 189)}          # colored grid 157-188
remove |= {"205", "206", "207", "208"}               # grey word-15 (rows 0-3)
remove |= {"249", "264", "266", "267", "268", "269", "270", "271"}  # "..." row
remove |= {"273", "274", "275", "276", "277", "278", "279", "280"}  # row 127
remove |= {"216", "189", "190", "191", "192", "193",
           "194", "195", "196", "197", "198"}        # col header group+labels
remove |= {"215", "229", "230", "231", "232", "233", "234", "235"}  # top I/O
# reference scaffolding cells the user added at bottom-left
remove |= {"340"}
remove |= {str(i) for i in [341, 353, 354, 355, 356, 357, 358, 359, 360, 361]}
remove |= {str(i) for i in range(372, 397)}          # 372-396

for cell in list(root):
    if cell.get('id') in remove:
        root.remove(cell)

# ---- widen the Dual Port 0 header (id 241) ------------------------------
for cell in root:
    if cell.get('id') == "241":
        g = cell.find('mxGeometry')
        g.set('x', '152')
        g.set('width', '772')

# ---- helpers ------------------------------------------------------------
def cell(cid, value, style, x, y, w, h):
    c = ET.SubElement(root, 'mxCell')
    c.set('id', cid); c.set('value', value); c.set('style', style)
    c.set('parent', '1'); c.set('vertex', '1')
    g = ET.SubElement(c, 'mxGeometry')
    g.set('x', str(round(x, 3))); g.set('y', str(round(y, 3)))
    g.set('width', str(round(w, 3))); g.set('height', str(round(h, 3)))
    g.set('as', 'geometry')

WORD_STYLE = ("rounded=0;whiteSpace=wrap;html=1;fillColor={fill};strokeColor=#FFFFFF;"
              "strokeWidth=2;fontSize=10;fontColor=#212121;align=center;verticalAlign=top;"
              "movable=1;resizable=1;rotatable=1;deletable=1;editable=1;locked=0;connectable=1;")
BYTE_STYLE = ("rounded=0;whiteSpace=wrap;html=1;fillColor={fill};strokeColor=#FFFFFF;"
              "strokeWidth=1;fontSize=9;fontColor=#212121;align=center;verticalAlign=middle;"
              "movable=1;resizable=1;rotatable=1;deletable=1;editable=1;locked=0;connectable=1;")
LABEL_H = 27.0

def word(rlabel, c, X, Y, fill, W=CW):
    lo = 8 * c
    val = "word ({},{})\n{}-{}".format(rlabel, c, lo, lo + 7)
    cell("wc_{}_{}".format(rlabel, c), val, WORD_STYLE.format(fill=fill), X, Y, W, CH)
    bw = W / 4.0
    bh = (CH - LABEL_H) / 2.0
    for i in range(8):
        col, sub = i % 4, i // 4
        cell("bt_{}_{}_{}".format(rlabel, c, i), str(lo + i),
             BYTE_STYLE.format(fill=fill),
             X + col * bw, Y + LABEL_H + sub * bh, bw, bh)

# ---- colored grid: rows 0-3, cols 0-10 ----------------------------------
for r in range(4):
    for c in range(11):
        word(str(r), c, XCOL(c), ROWY[r], BANK[(r + c) % 8])

# ---- grey word-15 boxes (rows 0-3) --------------------------------------
for r in range(4):
    word(str(r), 15, GX, ROWY[r], GREY, W=GW)

# ---- "..." skipped-rows row (y=466), cols 0-10 --------------------------
DOT_STYLE = ("rounded=0;whiteSpace=wrap;html=1;fillColor=light-dark(#cccccc, #543131);"
             "strokeColor=#FFFFFF;strokeWidth=2;fontSize=12;fontColor=#212121;align=center;"
             "verticalAlign=middle;movable=1;resizable=1;rotatable=1;deletable=1;editable=1;"
             "locked=0;connectable=1;")
for c in range(11):
    cell("dots_{}".format(c), "...", DOT_STYLE, XCOL(c), 466.0, CW, 25.0)

# ---- row 127 (greyed), cols 0-10 + grey word-15 -------------------------
for c in range(11):
    word("127", c, XCOL(c), 490.0, GREY)
word("127", 15, GX, 490.0, GREY, W=GW)

# ---- column headers -----------------------------------------------------
HDR_STYLE = ("text;html=1;align=center;verticalAlign=middle;fontSize=12;fontStyle=1;"
             "fontColor=#37474F;movable=1;resizable=1;rotatable=1;deletable=1;editable=1;"
             "locked=0;connectable=1;")
for c in range(11):
    cell("colh_{}".format(c), "col {}".format(c), HDR_STYLE, XCOL(c), 102.0, CW, 22.0)
cell("colh_dots", "...", HDR_STYLE, 932.5, 102.0, 35.27, 22.0)
cell("colh_15", "col 15", HDR_STYLE, GX, 102.0, GW, 22.0)

# ---- top I/O trapezoids, cols 0-10 --------------------------------------
IO_STYLE = ("shape=trapezoid;perimeter=trapezoidPerimeter;whiteSpace=wrap;html=1;fixedSize=1;"
            "size=10;movable=1;resizable=1;rotatable=1;deletable=1;editable=1;locked=0;connectable=1;")
for c in range(11):
    cell("io_{}".format(c), "I/O", IO_STYLE, XCOL(c) + 6, 160.0, 60.0, 19.43)

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
print("done: regenerated grid with 11 colored cols + grey word-15")
