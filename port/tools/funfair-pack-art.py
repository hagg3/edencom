#!/usr/bin/env python3
"""funfair-pack-art.py -- draws the tiles and icons of the Moof Funfair example mod pack.

  python3 -I port/tools/funfair-pack-art.py SRC_DIR PACK_DIR

SRC_DIR holds original 48x48 PNGs of Kenney's Voxel Pack (CC0 1.0, Kenney Vleugels, www.kenney.nl), as
mirrored in https://github.com/minetest-mods/voxelpack_by_kenney (master): the names in KENNEY below. Only the
48x48 Kenney files are used: that mirror also carries 16x16 and 128x128 files (wool, carts track) from other
sources, so the stripes, track, chequer and bulbs here are drawn by this script instead. Everything drawn here
(stripes, ticket window, cannon, flag, chequer, chalkboard, bullseye, bulbs, track, the five icons) is part of
Emod. Writes PACK_DIR/textures/*.png (32x32; opaque tiles flattened to RGB: the opaque atlas is uploaded
RGB565 and drops alpha) and PACK_DIR/icons/*.png (64x64 RGBA, white on transparent: the UI tints them).
Deterministic: the same inputs give the same bytes. Needs Pillow.
"""
import math
import os
import random
import sys

from PIL import Image, ImageDraw

KENNEY = ["default_wood.png", "default_fence_wood.png", "default_obsidian_block.png"]


def tile(src, name):
    """A Kenney tile as 32x32 RGBA."""
    im = Image.open(os.path.join(src, name)).convert("RGBA")
    if im.size != (48, 48):
        raise SystemExit("%s: expected 48x48, got %s" % (name, im.size))
    return im.resize((32, 32), Image.BOX)


def noise_fill(rgb, spread, seed, size=32):
    rnd = random.Random(seed)
    im = Image.new("RGBA", (size, size))
    px = im.load()
    for y in range(size):
        for x in range(size):
            d = rnd.randint(-spread, spread)
            px[x, y] = tuple(max(0, min(255, c + d)) for c in rgb) + (255,)
    return im


def shade(rgb, k):
    return tuple(max(0, min(255, int(c * k))) for c in rgb)


def stripes(a, b, width, seed, vertical=True):
    """Fabric: alternating a/b bands with a little weave noise."""
    rnd = random.Random(seed)
    im = Image.new("RGBA", (32, 32))
    px = im.load()
    for y in range(32):
        for x in range(32):
            band = ((x if vertical else y) // width) % 2
            base = a if band == 0 else b
            d = rnd.randint(-6, 6) + (4 if (x + y) % 2 == 0 else -4)
            px[x, y] = tuple(max(0, min(255, c + d)) for c in base) + (255,)
    return im


def frame(im, rgb, width=1):
    px = im.load()
    for i in range(32):
        for j in range(width):
            for (a, b) in ((i, j), (i, 31 - j), (j, i), (31 - j, i)):
                px[a, b] = rgb + (255,)
    return im


def rect(im, x0, y0, x1, y1, rgb):
    px = im.load()
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            px[x, y] = rgb + (255,)


RED, WHITE = (205, 45, 45), (238, 234, 225)


def booth_side(src):
    """A red/white scalloped awning over a ticket window in a plank wall."""
    im = tile(src, "default_wood.png")
    aw = stripes(RED, WHITE, 4, 11)
    px, apx = im.load(), aw.load()
    for y in range(11):
        for x in range(32):
            px[x, y] = apx[x, y]
    for x in range(32):   # scallops: each band ends in a rounded tip
        cx = (x // 4) * 4 + 1.5
        depth = 11 + int(2.2 - abs(x - cx))
        for y in range(11, depth + 1):
            px[x, y] = apx[x, y]
        px[x, min(31, depth + 1)] = (90, 55, 30, 255)   # shadow under the awning
    rect(im, 8, 16, 23, 26, (40, 30, 25))           # the window
    rect(im, 9, 17, 22, 25, (70, 55, 45))
    rect(im, 13, 19, 18, 23, (245, 205, 70))        # a ticket in it
    rect(im, 14, 20, 17, 22, (215, 150, 40))
    rect(im, 6, 27, 25, 28, (120, 75, 40))          # the counter
    return im


def canvas_grey():
    # Paint multiplies the grey: light bands near white, dark bands a step down, so any colour reads as stripes.
    return stripes((242, 242, 242), (178, 178, 178), 4, 21)


def canvas_colour():
    return stripes(RED, WHITE, 4, 21)


def metal(src):
    """Dark gunmetal from Kenney's obsidian block, lifted a little so it isn't black."""
    im = tile(src, "default_obsidian_block.png")
    px = im.load()
    for y in range(32):
        for x in range(32):
            r, g, b, a = px[x, y]
            v = 0.3 * r + 0.59 * g + 0.11 * b
            px[x, y] = (int(55 + v * 0.55), int(60 + v * 0.55), int(72 + v * 0.6), 255)
    return im


def cannon_side(src):
    im = metal(src)
    for y in (5, 6, 25, 26):                        # two brass bands
        rect(im, 0, y, 31, y, (200, 160, 60) if y in (5, 25) else (150, 110, 40))
    px = im.load()
    for x in range(3, 30, 7):                       # rivets
        for y in (3, 28):
            px[x, y] = (210, 210, 220, 255)
            px[x + 1, y + 1] = (35, 35, 45, 255)
    # a star on the barrel
    for (x, y) in ((16, 11), (15, 12), (16, 12), (17, 12), (12, 14), (13, 14), (14, 14), (15, 14), (16, 14), (17, 14),
                   (18, 14), (19, 14), (20, 14), (14, 15), (15, 15), (16, 15), (17, 15), (18, 15), (15, 16), (16, 16),
                   (17, 16), (14, 17), (15, 17), (17, 17), (18, 17), (13, 18), (14, 18), (18, 18), (19, 18)):
        px[x, y] = (245, 205, 70, 255)
    return im


def cannon_top(src):
    im = metal(src)
    px = im.load()
    for y in range(32):
        for x in range(32):
            d = math.hypot(x - 15.5, y - 15.5)
            if d < 7:
                px[x, y] = (12, 12, 16, 255)        # the bore
            elif d < 9.5:
                px[x, y] = (200, 160, 60, 255)      # brass muzzle ring
            elif d < 10.5:
                px[x, y] = (130, 95, 35, 255)
    return frame(im, (40, 42, 50), 1)


def gate_side(src):
    """Planks with a pennant flag on a pole."""
    im = tile(src, "default_wood.png")
    rect(im, 8, 4, 9, 29, (70, 45, 25))             # pole
    rect(im, 7, 3, 10, 4, (245, 205, 70))           # finial
    px = im.load()
    for y in range(6, 18):                          # a pennant pointing right
        reach = 10 + int((6 - abs(y - 11.5)) * 2.6)
        for x in range(10, reach):
            px[x, y] = ((60, 120, 215) if y < 12 else (45, 95, 185)) + (255,)
    rect(im, 4, 29, 13, 30, (90, 60, 35))           # base
    return im


def chequer():
    im = Image.new("RGBA", (32, 32))
    px = im.load()
    for y in range(32):
        for x in range(32):
            dark = ((x // 8) + (y // 8)) % 2 == 1
            v = 30 if dark else 240
            v += 6 if (x * 7 + y * 3) % 5 == 0 else 0
            px[x, y] = (v, v, v, 255)
    return im


def board(src):
    """A dark green chalkboard in a wooden frame."""
    im = noise_fill((40, 78, 52), 5, 31)
    wood = tile(src, "default_wood.png").load()
    px = im.load()
    for y in range(32):
        for x in range(32):
            if x < 3 or y < 3 or x > 28 or y > 28:
                px[x, y] = wood[x, y]
    for (x0, y0, x1) in ((6, 8, 20), (6, 13, 24), (6, 18, 16), (6, 23, 22)):   # chalk smudges
        for x in range(x0, x1):
            if (x * 5) % 7 != 0:
                px[x, y0] = (150, 175, 155, 255)
    return frame(im, (60, 38, 20), 1)


def target_top():
    im = Image.new("RGBA", (32, 32))
    px = im.load()
    for y in range(32):
        for x in range(32):
            d = math.hypot(x - 15.5, y - 15.5)
            ring = int(d / 3.2)
            if d >= 16:
                c = (238, 234, 225)
            elif ring == 0:
                c = (245, 205, 60)                  # bullseye
            else:
                c = RED if ring % 2 == 1 else WHITE
            px[x, y] = c + (255,)
    return im


def target_side():
    im = stripes(RED, WHITE, 4, 41, vertical=False)
    return frame(im, (140, 30, 30), 1)


def lamp_off(src):
    """Unlit bulbs on a dark panel."""
    im = metal(src)
    px = im.load()
    for cy in (8, 23):
        for cx in (8, 23):
            for y in range(32):
                for x in range(32):
                    d = math.hypot(x - cx + 0.5, y - cy + 0.5)
                    if d < 5:
                        k = 1.0 - d / 9.0
                        px[x, y] = (int(200 * k), int(195 * k), int(170 * k), 255)
            px[cx - 2, cy - 2] = (255, 255, 240, 255)   # glint
    return im


def track_top():
    """A packed-earth race lane with a white line down each edge."""
    im = noise_fill((176, 132, 88), 12, 51)
    px = im.load()
    rnd = random.Random(52)
    for _ in range(40):                              # pebbles
        x, y = rnd.randrange(32), rnd.randrange(32)
        px[x, y] = (130, 98, 66, 255)
    for y in range(32):
        for x in (1, 2, 29, 30):
            px[x, y] = (240, 238, 228, 255)
    return im


def rail(src):
    """A lane divider: two white-painted boards across dark fence wood, posts at the edges."""
    im = tile(src, "default_fence_wood.png")
    px = im.load()
    for y in range(32):
        for x in range(32):
            r, g, b, a = px[x, y]
            if 5 <= y <= 11 or 19 <= y <= 25:
                v = 200 + (r - 150) // 6
                px[x, y] = (v + 30, v + 28, v + 20, 255)
            else:
                px[x, y] = (int(r * 0.55), int(g * 0.55), int(b * 0.55), 255)
    for y in (12, 26):
        rect(im, 0, y, 31, y, (45, 30, 18))
    for x0 in (0, 29):
        rect(im, x0, 0, x0 + 2, 31, (95, 62, 34))
    return im


# ---- icons: drawn at 4x with strokes like the built-in Lucide set, then downsampled to 64x64 ----
S = 256
W = 18   # stroke width at 4x (Lucide's 2 px at 24 -> ~5 px at 64)


def canvas():
    im = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    return im, ImageDraw.Draw(im)


def line(d, pts, w=W):
    d.line(pts, fill=(255, 255, 255, 255), width=w, joint="curve")
    for p in (pts[0], pts[-1]):
        d.ellipse((p[0] - w / 2, p[1] - w / 2, p[0] + w / 2, p[1] + w / 2), fill=(255, 255, 255, 255))


def icon_tent():
    im, d = canvas()
    line(d, [(128, 30), (128, 52)])                             # pole tip
    line(d, [(128, 52), (28, 210), (228, 210), (128, 52)])      # big top
    line(d, [(128, 52), (90, 210)])
    line(d, [(128, 52), (166, 210)])
    line(d, [(106, 210), (128, 150), (150, 210)])               # the door
    d.polygon([(128, 22), (176, 36), (128, 50)], fill=(255, 255, 255, 255))   # pennant
    return im


def icon_ticket():
    im, d = canvas()
    x0, y0, x1, y1 = 26, 70, 230, 186
    pts = [(x0, y0), (x1, y0), (x1, 106), (x1 - 22, 128), (x1, 150), (x1, y1), (x0, y1), (x0, 150), (x0 + 22, 128),
           (x0, 106), (x0, y0)]
    line(d, pts)
    for y in (96, 128, 160):                                    # perforation
        d.ellipse((160 - 7, y - 7, 160 + 7, y + 7), fill=(255, 255, 255, 255))
    line(d, [(64, 112), (124, 112)], 14)
    line(d, [(64, 146), (110, 146)], 14)
    return im


def icon_trophy():
    im, d = canvas()
    line(d, [(70, 40), (186, 40), (186, 110)])
    d.arc((70, 40, 186, 170), 0, 180, fill=(255, 255, 255, 255), width=W)
    line(d, [(70, 40), (70, 110)])
    d.arc((22, 52, 82, 122), 90, 270, fill=(255, 255, 255, 255), width=W)    # handles
    d.arc((174, 52, 234, 122), 270, 90, fill=(255, 255, 255, 255), width=W)
    line(d, [(128, 170), (128, 196)])
    line(d, [(82, 216), (174, 216)])
    line(d, [(96, 196), (160, 196)])
    return im


def icon_bulb():
    im, d = canvas()
    d.arc((66, 26, 190, 150), 140, 400, fill=(255, 255, 255, 255), width=W)
    line(d, [(80, 128), (100, 168), (156, 168), (176, 128)])
    line(d, [(100, 192), (156, 192)])
    line(d, [(110, 220), (146, 220)])
    line(d, [(128, 120), (128, 168)], 12)
    return im


def icon_cannon():
    im, d = canvas()
    # the barrel, tilted up to the right
    a = math.radians(-28)
    cx, cy = 110, 128
    L, R = 108, 34
    def rot(x, y):
        return (cx + x * math.cos(a) - y * math.sin(a), cy + x * math.sin(a) + y * math.cos(a))
    body = [rot(-70, -R + 6), rot(L, -R), rot(L, R), rot(-70, R - 6)]
    line(d, body + [body[0]])
    line(d, [rot(L - 22, -R), rot(L - 22, R)], 12)
    d.ellipse((60, 140, 140, 220), outline=(255, 255, 255, 255), width=W)    # wheel
    d.ellipse((92, 172, 108, 188), fill=(255, 255, 255, 255))
    for (x, y, r) in ((214, 44, 12), (238, 74, 8), (196, 22, 7)):          # puffs
        d.ellipse((x - r, y - r, x + r, y + r), fill=(255, 255, 255, 255))
    return im


ICONS = {"tent": icon_tent, "ticket": icon_ticket, "trophy": icon_trophy, "bulb": icon_bulb, "cannon": icon_cannon}


def build(src):
    t = {}
    t["booth_side"] = booth_side(src)
    t["canvas"] = canvas_grey()
    t["canvas_color"] = canvas_colour()
    t["cannon_side"] = cannon_side(src)
    t["cannon_top"] = cannon_top(src)
    t["gate_side"] = gate_side(src)
    t["chequer"] = chequer()
    t["board"] = board(src)
    t["target_top"] = target_top()
    t["target_side"] = target_side()
    t["lamp_off"] = lamp_off(src)
    t["track_top"] = track_top()
    t["rail"] = rail(src)
    t["planks"] = tile(src, "default_wood.png")
    return t


def main(src, pack):
    for name in KENNEY:
        if not os.path.isfile(os.path.join(src, name)):
            raise SystemExit("missing %s in %s" % (name, src))
    tex, ico = os.path.join(pack, "textures"), os.path.join(pack, "icons")
    os.makedirs(tex, exist_ok=True)
    os.makedirs(ico, exist_ok=True)
    tiles = build(src)
    for name, im in tiles.items():
        flat = Image.new("RGBA", im.size, (0, 0, 0, 255))
        flat.alpha_composite(im)
        flat.convert("RGB").save(os.path.join(tex, name + ".png"), optimize=True)
    for name, fn in ICONS.items():
        im = fn().resize((64, 64), Image.LANCZOS)
        a = im.getchannel("A")
        out = Image.merge("RGBA", (Image.new("L", (64, 64), 255),) * 3 + (a,))
        out.save(os.path.join(ico, name + ".png"), optimize=True)
    print("wrote %d tiles to %s and %d icons to %s" % (len(tiles), tex, len(ICONS), ico))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
