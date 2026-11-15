#!/usr/bin/env python3
"""One-off: bake the three 88x31 badges (plan D15) into site/static/img/. Needs Pillow (not a project
dependency; the PNGs are committed). Run: python3 site/tools/make-badges.py (from the repo root)."""
from PIL import Image, ImageDraw, ImageFont
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "site/static/img"
FONT = str(ROOT / "media/fonts/Jersey10-Regular.ttf")
BADGES = [("b88-eden", "EDEN CE", None, (255, 255, 255)),
          ("b88-gpl", "GPLv3", (164, 42, 42), (255, 255, 255)),
          ("b88-discord", "Discord", (88, 101, 242), (255, 255, 255)),
          ("b88-github", "GitHub", (36, 41, 47), (255, 255, 255))]


def make(name, label, bg, fg):
    im = Image.new("RGB", (88, 31), bg or (0, 0, 0))
    if bg is None:  # grass tile
        tile = Image.open(OUT / "tile-grass.png").convert("RGB").resize((16, 16), Image.NEAREST)
        for x in range(0, 88, 16):
            for y in range(0, 31, 16):
                im.paste(tile, (x, y))
    d = ImageDraw.Draw(im)
    # One label, as large as fits the 88x31 face (inside the bevel), centred both ways.
    size = 30
    while size > 8:
        font = ImageFont.truetype(FONT, size)
        l, t, r, b = d.textbbox((0, 0), label, font=font)
        if r - l <= 80 and b - t <= 23:
            break
        size -= 1
    x = round((88 - (r - l)) / 2) - l
    y = round((31 - (b - t)) / 2) - t
    d.text((x + 1, y + 1), label, font=font, fill=(0, 0, 0))
    d.text((x, y), label, font=font, fill=fg)
    # bevel: light top-left, dark bottom-right, black keyline
    d.rectangle((0, 0, 87, 30), outline=(0, 0, 0))
    d.line((1, 1, 86, 1), fill=(255, 255, 255)); d.line((1, 1, 1, 29), fill=(255, 255, 255))
    d.line((1, 29, 86, 29), fill=(60, 60, 60)); d.line((86, 1, 86, 29), fill=(60, 60, 60))
    im = im.quantize(32)
    im.save(OUT / f"{name}.png", optimize=True)


for b in BADGES:
    make(*b)
