#!/usr/bin/env python3
# D.2t: grow the block atlases from 32 to 64 tiles and add the art for ids 112-127.
#
#   python3 -I web/tools/atlas-d2t.py WORKING/atlas-2026.png media/textures
#
# The source is the 2026 game's atlas (32 x 4096, 128 tiles). Its tiles 58-73 are 112-127 as
# drawn unpainted (the art carries its colour) and 87-102 are the same 16 in greyscale (the paint
# multiplies onto them) -- id order in both runs, matched by pattern. Our atlas keeps stock tiles
# 0-31 untouched and appends them as 32-47 (TEX_NEWBLOCK_COLOR) and 48-63 (TEX_NEWBLOCK).
# atlas2 grows to 64 tiles of transparent padding so both atlases share the 1/64 texture matrix.
# Idempotent: tiles 0-31 are always read from the first 1024 rows of the current files.
import sys, os
from PIL import Image

T = 32
SRC_COLOR, SRC_GREY, N = 58, 87, 16

def main(src, outdir):
    g = Image.open(src).convert("RGBA")
    if g.size != (T, 128 * T):
        sys.exit(f"{src}: expected 32x4096, got {g.size}")
    a1 = Image.open(os.path.join(outdir, "atlas.png")).convert("RGBA").crop((0, 0, T, 32 * T))
    a2 = Image.open(os.path.join(outdir, "atlas2.png")).convert("RGBA").crop((0, 0, T, 32 * T))

    out1 = Image.new("RGBA", (T, 64 * T), (0, 0, 0, 255))
    out1.paste(a1, (0, 0))
    for i in range(N):
        for dst, base in ((32 + i, SRC_COLOR + i), (48 + i, SRC_GREY + i)):
            tile = g.crop((0, base * T, T, base * T + T))
            tile.putalpha(255)          # atlas loads as RGB565; two source tiles carry a soft edge alpha
            out1.paste(tile, (0, dst * T))
    out1.save(os.path.join(outdir, "atlas.png"))

    out2 = Image.new("RGBA", (T, 64 * T), (0, 0, 0, 0))
    out2.paste(a2, (0, 0))
    out2.save(os.path.join(outdir, "atlas2.png"))

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
