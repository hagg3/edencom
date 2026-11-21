#!/usr/bin/env python3
"""skycastle-pack-art.py -- draws the tiles and icons of the Sky Castle example mod pack.

  python3 -I port/tools/skycastle-pack-art.py PACK_DIR

Every tile and icon is drawn here from shapes and seeded noise (no source images), and is part of Emod.
Writes PACK_DIR/textures/*.png (32x32; opaque tiles flattened to RGB, because the opaque atlas is uploaded RGB565
and drops alpha; the one transparent tile, vine, keeps its alpha) and PACK_DIR/icons/*.png (64x64 RGBA, white on
transparent: the UI tints them). Deterministic: a rerun gives the same bytes. Needs Pillow.
"""
import math
import os
import random
import sys

from PIL import Image, ImageDraw

TRANSPARENT = {"vine"}


def clamp(v):
    return max(0, min(255, int(v)))


def noise_fill(rgb, spread, seed):
    rnd = random.Random(seed)
    im = Image.new("RGBA", (32, 32))
    px = im.load()
    for y in range(32):
        for x in range(32):
            d = rnd.randint(-spread, spread)
            px[x, y] = tuple(clamp(c + d) for c in rgb) + (255,)
    return im


def shade(rgb, k):
    return tuple(clamp(c * k) for c in rgb)


def rect(im, x0, y0, x1, y1, rgb, a=255):
    px = im.load()
    for y in range(max(0, y0), min(31, y1) + 1):
        for x in range(max(0, x0), min(31, x1) + 1):
            px[x, y] = tuple(rgb) + (a,)


def frame(im, rgb, width=1):
    rect(im, 0, 0, 31, width - 1, rgb)
    rect(im, 0, 32 - width, 31, 31, rgb)
    rect(im, 0, 0, width - 1, 31, rgb)
    rect(im, 32 - width, 0, 31, 31, rgb)
    return im


def bricks(base, mortar, seed):
    """Castle stone: four courses of offset blocks, each block its own shade, bevelled."""
    rnd = random.Random(seed)
    im = noise_fill(base, 7, seed)
    px = im.load()
    for row in range(4):
        y0 = row * 8
        off = 0 if row % 2 == 0 else 8
        for col in range(-1, 3):
            x0 = col * 16 + off
            k = 0.9 + rnd.random() * 0.2
            for y in range(y0, y0 + 8):
                for x in range(x0, x0 + 16):
                    if not (0 <= x < 32):
                        continue
                    r, g, b, _ = px[x, y]
                    c = (r * k, g * k, b * k)
                    if y == y0 + 7 or x == x0 + 15:
                        c = mortar
                    elif y == y0 or x == x0:
                        c = tuple(v * 1.12 for v in c)       # lit top/left edge
                    elif y == y0 + 6 or x == x0 + 14:
                        c = tuple(v * 0.82 for v in c)       # shaded bottom/right edge
                    px[x, y] = tuple(clamp(v) for v in c) + (255,)
    return im


STONE = (176, 168, 150)
STONE_MORTAR = (96, 90, 80)


def brick_grey():
    # Paint tints grey art: an unpainted castle_brick swaps to brick_color (the "unpainted" twin).
    return bricks((178, 178, 178), (92, 92, 92), 11)


def brick_colour():
    return bricks(STONE, STONE_MORTAR, 11)


def keystone_side():
    """Castle stone with a gold tower emblem: the block that opens the menu."""
    im = bricks(STONE, STONE_MORTAR, 12)
    gold, dark = (236, 190, 60), (120, 84, 20)
    rect(im, 7, 6, 24, 27, dark)
    rect(im, 8, 7, 23, 26, (60, 70, 110))
    # tower: battlements, body, door
    for x0 in (10, 14, 18):
        rect(im, x0, 9, x0 + 2, 11, gold)
    rect(im, 10, 12, 21, 24, gold)
    rect(im, 14, 18, 17, 24, (60, 70, 110))
    rect(im, 15, 14, 16, 15, (60, 70, 110))
    return im


def keystone_top():
    im = bricks(STONE, STONE_MORTAR, 13)
    d = ImageDraw.Draw(im)
    d.polygon([(16, 5), (27, 16), (16, 27), (5, 16)], fill=(120, 84, 20, 255))
    d.polygon([(16, 8), (24, 16), (16, 24), (8, 16)], fill=(236, 190, 60, 255))
    d.polygon([(16, 12), (20, 16), (16, 20), (12, 16)], fill=(255, 236, 150, 255))
    return im


def pad(rgb, seed):
    im = noise_fill(rgb, 6, seed)
    frame(im, shade(rgb, 0.55), 2)
    rect(im, 2, 2, 29, 2, shade(rgb, 1.25))
    rect(im, 2, 2, 2, 29, shade(rgb, 1.25))
    return im


def start_top():
    """A green pad with a white arrow pointing the way."""
    im = pad((60, 168, 70), 21)
    d = ImageDraw.Draw(im)
    d.polygon([(16, 6), (26, 17), (20, 17), (20, 26), (12, 26), (12, 17), (6, 17)], fill=(245, 245, 240, 255))
    return im


def flag_top():
    """A blue checkpoint pad with a flag on a pole."""
    im = pad((60, 110, 200), 22)
    rect(im, 9, 6, 10, 26, (235, 235, 235))
    d = ImageDraw.Draw(im)
    d.polygon([(11, 6), (25, 10), (11, 15)], fill=(250, 210, 60, 255))
    rect(im, 6, 26, 13, 27, (40, 60, 120))
    return im


def chequer():
    im = Image.new("RGBA", (32, 32))
    px = im.load()
    rnd = random.Random(23)
    for y in range(32):
        for x in range(32):
            on = ((x // 8) + (y // 8)) % 2 == 0
            base = (238, 236, 230) if on else (28, 28, 30)
            d = rnd.randint(-5, 5)
            px[x, y] = tuple(clamp(c + d) for c in base) + (255,)
    return frame(im, (200, 40, 40), 2)


def spring():
    """A puffy white cloud on sky blue, with a coil showing through: bouncy."""
    im = noise_fill((120, 180, 235), 5, 24)
    d = ImageDraw.Draw(im)
    for (x, y, r) in ((9, 13, 7), (17, 10, 8), (24, 14, 7), (13, 19, 7), (21, 20, 7)):
        d.ellipse((x - r, y - r, x + r, y + r), fill=(250, 250, 255, 255))
    for (x, y, r) in ((10, 15, 4), (19, 13, 4), (22, 19, 3)):
        d.ellipse((x - r, y - r, x + r, y + r), fill=(226, 232, 245, 255))
    for i in range(3):                                   # the coil peeking out at the bottom
        y = 25 + i * 2
        d.line([(10, y), (22, y + 1)], fill=(150, 150, 165, 255), width=1)
    return im


def vine():
    """Leafy vines on a transparent background (the transparent pass)."""
    im = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    rnd = random.Random(25)
    for x0 in (5, 16, 26):
        pts = [(x0 + int(2 * math.sin(y / 4.0 + x0)), y) for y in range(0, 32, 2)]
        d.line(pts, fill=(70, 110, 40, 255), width=2)
        for y in range(1, 32, 5):
            lx = x0 + int(2 * math.sin(y / 4.0 + x0))
            side = 1 if (y // 5 + x0) % 2 == 0 else -1
            g = (60 + rnd.randint(0, 40), 150 + rnd.randint(0, 50), 50 + rnd.randint(0, 20), 255)
            d.ellipse((lx + side * 1, y, lx + side * 1 + side * 4, y + 3) if side > 0 else
                      (lx - 5, y, lx - 1, y + 3), fill=g)
    return im


def ice():
    """Pale glassy blue with light streaks and a crack: slippery."""
    im = noise_fill((170, 215, 240), 4, 26)
    d = ImageDraw.Draw(im)
    for (x, y) in ((3, 26), (10, 30), (18, 22)):
        d.line([(x, y), (x + 10, y - 10)], fill=(235, 248, 255, 255), width=2)
    d.line([(20, 4), (24, 9), (22, 14), (27, 19)], fill=(120, 170, 205, 255), width=1)
    return frame(im, (140, 190, 225), 1)


def spikes_top():
    """Rows of steel spikes on a dark plate."""
    im = noise_fill((60, 56, 56), 6, 27)
    d = ImageDraw.Draw(im)
    for row in range(4):
        for col in range(4):
            cx, cy = col * 8 + 4 + (row % 2) * 2 - 1, row * 8 + 4
            d.polygon([(cx - 3, cy + 3), (cx + 3, cy + 3), (cx, cy - 3)], fill=(205, 210, 215, 255))
            d.line([(cx, cy - 3), (cx + 3, cy + 3)], fill=(120, 124, 130, 255))
            d.point((cx, cy - 2), fill=(200, 40, 40, 255))
    return frame(im, (150, 30, 30), 1)


def fountain_top():
    """A ring of stone round rippling water."""
    im = bricks(STONE, STONE_MORTAR, 28)
    d = ImageDraw.Draw(im)
    d.ellipse((3, 3, 28, 28), fill=(140, 132, 118, 255))
    d.ellipse((6, 6, 25, 25), fill=(50, 120, 210, 255))
    for r in (3, 6):
        d.ellipse((15 - r, 15 - r, 16 + r, 16 + r), outline=(170, 215, 250, 255))
    d.ellipse((13, 13, 18, 18), fill=(235, 245, 255, 255))
    # a red cross: it heals
    rect(im, 14, 1, 17, 6, (220, 40, 40))
    rect(im, 13, 2, 18, 5, (220, 40, 40))
    return im


def hatch_top():
    """A dark iron hatch with a black-and-yellow warning edge: bombs come from here."""
    im = noise_fill((54, 54, 60), 5, 29)
    px = im.load()
    for y in range(32):
        for x in range(32):
            if x < 3 or y < 3 or x > 28 or y > 28:
                px[x, y] = ((240, 200, 30) if ((x + y) // 3) % 2 == 0 else (30, 30, 30)) + (255,)
    d = ImageDraw.Draw(im)
    d.ellipse((10, 11, 22, 23), fill=(20, 20, 22, 255))         # a bomb
    d.line([(19, 12), (23, 7)], fill=(200, 170, 120, 255), width=2)
    d.point((24, 6), fill=(255, 140, 30, 255))
    d.point((13, 14), fill=(120, 120, 130, 255))
    return im


def board():
    """A slate scoreboard in a gold frame (signs hang on it)."""
    im = noise_fill((48, 56, 70), 5, 30)
    frame(im, (196, 156, 60), 2)
    rect(im, 2, 2, 29, 2, (236, 200, 100))
    for y in (9, 15, 21):
        rect(im, 6, y, 25, y, (90, 100, 118))
    return im


def build():
    return {
        "brick": brick_grey(),
        "brick_color": brick_colour(),
        "keystone_side": keystone_side(),
        "keystone_top": keystone_top(),
        "start_top": start_top(),
        "flag_top": flag_top(),
        "chequer": chequer(),
        "spring": spring(),
        "vine": vine(),
        "ice": ice(),
        "spikes_top": spikes_top(),
        "fountain_top": fountain_top(),
        "hatch_top": hatch_top(),
        "board": board(),
    }


# ---- Icons: drawn at 256 px with Lucide-like strokes, downsampled to 64 ----
S = 256
W = 18


def canvas():
    im = Image.new("RGBA", (S, S), (255, 255, 255, 0))
    return im, ImageDraw.Draw(im)


def line(d, pts, w=W):
    d.line(pts, fill=(255, 255, 255, 255), width=w, joint="curve")
    for p in (pts[0], pts[-1]):
        d.ellipse((p[0] - w / 2, p[1] - w / 2, p[0] + w / 2, p[1] + w / 2), fill=(255, 255, 255, 255))


def icon_castle():
    im, d = canvas()
    # two towers with battlements and a gate between them
    line(d, [(40, 220), (40, 60), (60, 60), (60, 80), (80, 80), (80, 60), (100, 60), (100, 120),
             (156, 120), (156, 60), (176, 60), (176, 80), (196, 80), (196, 60), (216, 60), (216, 220), (40, 220)])
    line(d, [(104, 220), (104, 176)])
    d.arc((104, 150, 152, 200), 180, 360, fill=(255, 255, 255, 255), width=W)
    line(d, [(152, 220), (152, 176)])
    line(d, [(128, 120), (128, 30)], 12)
    d.polygon([(132, 28), (184, 44), (132, 62)], fill=(255, 255, 255, 255))
    return im


def icon_stopwatch():
    im, d = canvas()
    d.ellipse((44, 60, 212, 228), outline=(255, 255, 255, 255), width=W)
    line(d, [(104, 30), (152, 30)])
    line(d, [(128, 30), (128, 60)])
    line(d, [(128, 144), (128, 96)])
    line(d, [(128, 144), (166, 118)])
    line(d, [(196, 64), (214, 82)])
    return im


def icon_flag():
    im, d = canvas()
    line(d, [(64, 232), (64, 28)])
    line(d, [(64, 40), (110, 26), (150, 52), (200, 40), (200, 140), (150, 152), (110, 126), (64, 140)])
    return im


ICONS = {"castle": icon_castle, "stopwatch": icon_stopwatch, "flag": icon_flag}


def main(pack):
    tex, ico = os.path.join(pack, "textures"), os.path.join(pack, "icons")
    os.makedirs(tex, exist_ok=True)
    os.makedirs(ico, exist_ok=True)
    tiles = build()
    for name, im in tiles.items():
        if name in TRANSPARENT:
            im.save(os.path.join(tex, name + ".png"), optimize=True)
            continue
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
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
