#!/usr/bin/env python3
# K.1-3: tile 65 of atlas.png (TEX_MISSING) is the "missing" block: a magenta/black checker, 4 x 4 checks of
# 8 px. A type byte >= 128 whose palette entry has no loaded definition draws it on every face (Classes/
# BlockRegistry.cpp; plan D4). Its blockColor is magenta too, so the unpainted multiply keeps it magenta.
#
#   python3 -I port/tools/atlas-k13.py media/textures
#
# Idempotent: every other pixel of atlas.png is kept; atlas2.png is not touched.
import sys, os
from PIL import Image

T = 32
TILE = 65

def main(d):
    p = os.path.join(d, "atlas.png")
    a = Image.open(p).convert("RGBA")
    if a.size != (T, 128 * T):
        sys.exit("atlas.png: expected 32x4096 (128 tiles)")
    px = a.load()
    for y in range(T):
        for x in range(T):
            px[x, TILE * T + y] = (255, 0, 255, 255) if ((x // 8) + (y // 8)) % 2 == 0 else (0, 0, 0, 255)
    a.save(p)

if __name__ == "__main__":
    main(sys.argv[1])
