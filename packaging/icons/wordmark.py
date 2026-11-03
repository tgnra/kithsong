"""Outlines "Kith" + "song" in Merienda Bold as SVG path data, shaped with HarfBuzz, for
generate.sh. Run it only to change the word's case or weight; it needs fontTools and
uharfbuzz, and Merienda's variable font from github.com/google/fonts (ofl/merienda):

    nix shell --impure --expr 'with import <nixpkgs> {}; python3.withPackages (p: [ p.fonttools p.uharfbuzz ])' \
      -c python3 packaging/icons/wordmark.py 'Merienda[wght].ttf' packaging/icons/wordmark-paths.txt

Writes three lines: "Kith"'s path, "song"'s path, and the word's "minx miny maxx maxy", in
font units (1000 to the em) with y pointing down and the baseline at 0. generate.sh places
the word for those bounds (75, -926, 4306, 329): change its transforms if they move.
"""
import sys
from io import BytesIO

import uharfbuzz as hb
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

src, out = sys.argv[1], sys.argv[2]
buf = BytesIO()
instantiateVariableFont(TTFont(src), {"wght": 700}).save(buf)
data = buf.getvalue()
font = TTFont(BytesIO(data))
glyphs = font.getGlyphSet()
order = font.getGlyphOrder()

shaped = hb.Buffer()
shaped.add_str("Kithsong")
shaped.guess_segment_properties()
hb.shape(hb.Font(hb.Face(data)), shaped, {"kern": True, "liga": True})

parts = {"kith": SVGPathPen(glyphs), "song": SVGPathPen(glyphs)}
bounds = BoundsPen(glyphs)
x = 0
for info, pos in zip(shaped.glyph_infos, shaped.glyph_positions):
    name = order[info.codepoint]
    flip = (1, 0, 0, -1, x + pos.x_offset, -pos.y_offset)
    glyphs[name].draw(TransformPen(parts["kith" if info.cluster < 4 else "song"], flip))
    glyphs[name].draw(TransformPen(bounds, flip))
    x += pos.x_advance

with open(out, "w") as f:
    f.write(parts["kith"].getCommands() + "\n")
    f.write(parts["song"].getCommands() + "\n")
    f.write(" ".join(f"{v:g}" for v in bounds.bounds) + "\n")
