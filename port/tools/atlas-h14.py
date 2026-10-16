#!/usr/bin/env python3
# H.14: grow the block atlases from 64 to 128 tiles and add tile 64 (TEX_GRASS_FLOWER), the 1.x
# flower patch from media/textures/grass3o.png (full colour, 32x32).
#
#   python3 -I port/tools/atlas-h14.py media/textures
#
# Idempotent: tiles 0-63 are always read from the first 2048 rows of the current files, 64 is rewritten.
import sys, os
from PIL import Image

T = 32

def main(d):
    flower = Image.open(os.path.join(d, "grass3o.png")).convert("RGBA")
    if flower.size != (T, T):
        sys.exit("grass3o.png: expected 32x32")
    flower.putalpha(255)                # atlas loads as RGB565
    a1 = Image.open(os.path.join(d, "atlas.png")).convert("RGBA").crop((0, 0, T, 64 * T))
    out1 = Image.new("RGBA", (T, 128 * T), (0, 0, 0, 255))
    out1.paste(a1, (0, 0))
    out1.paste(flower, (0, 64 * T))
    out1.save(os.path.join(d, "atlas.png"))
    a2 = Image.open(os.path.join(d, "atlas2.png")).convert("RGBA").crop((0, 0, T, 64 * T))
    out2 = Image.new("RGBA", (T, 128 * T), (0, 0, 0, 0))
    out2.paste(a2, (0, 0))
    out2.save(os.path.join(d, "atlas2.png"))

if __name__ == "__main__":
    main(sys.argv[1])
