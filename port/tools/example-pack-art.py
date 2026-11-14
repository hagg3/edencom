#!/usr/bin/env python3
"""example-pack-art.py -- draws the PNG tiles of the bundled example mod pack (K.2-4).

  python3 port/tools/example-pack-art.py port/tools/fixtures/packs/emod_example/textures

Procedural 32x32 art, written once and committed; kept so the tiles can be redrawn. Stdlib only.
"""
import os
import struct
import sys
import zlib


def png(path, w, h, px, alpha):
    """px(x, y) -> (r, g, b, a). Colour type 6 (RGBA) or 2 (RGB), 8-bit, no interlace."""
    rows = b""
    for y in range(h):
        rows += b"\0"
        for x in range(w):
            r, g, b, a = px(x, y)
            rows += bytes((r, g, b, a) if alpha else (r, g, b))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6 if alpha else 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def noise(x, y, s):
    v = (x * 374761393 + y * 668265263 + s * 2246822519) & 0xffffffff
    v = ((v ^ (v >> 13)) * 1274126177) & 0xffffffff
    return (v >> 24) / 255.0


def planks(x, y, base):
    row = y // 8
    seam = y % 8 == 7 or (x + row * 11) % 32 == 0
    grain = 0.85 + 0.15 * noise(x // 3, y, row)
    k = 0.55 if seam else grain
    return tuple(min(255, int(c * k)) for c in base) + (255,)


def main(out):
    os.makedirs(out, exist_ok=True)
    # oak: greyscale art (a painted block tints it); oak_color: its unpainted twin
    png(os.path.join(out, "oak.png"), 32, 32, lambda x, y: planks(x, y, (225, 225, 225)), False)
    png(os.path.join(out, "oak_color.png"), 32, 32, lambda x, y: planks(x, y, (196, 150, 96)), False)

    def rope(x, y):
        rail = x in (4, 5, 26, 27)
        rung = y % 8 in (3, 4) and 4 <= x <= 27
        if rail or rung:
            k = 0.8 + 0.2 * noise(x, y, 7)
            return (int(170 * k), int(130 * k), int(70 * k), 255)
        return (40, 34, 28, 255)
    png(os.path.join(out, "rope.png"), 32, 32, rope, False)

    def jelly(x, y):
        d = ((x - 11) ** 2 + (y - 10) ** 2) ** 0.5
        hi = 60 if d < 4 else 0
        return (min(255, 70 + hi), min(255, 200 + hi // 2), min(255, 90 + hi), 255)
    png(os.path.join(out, "jelly.png"), 32, 32, jelly, False)

    def vault(x, y):
        edge = x in (0, 31) or y in (0, 31)
        rivet = (x % 15 in (3, 4)) and (y % 15 in (3, 4))
        v = 70 + int(20 * noise(x, y, 3))
        if edge:
            v = 40
        if rivet:
            v = 150
        return (v, v, v + 8, 255)
    png(os.path.join(out, "vault.png"), 32, 32, vault, False)

    def stained(x, y):
        if x % 16 in (0, 15) or y % 16 in (0, 15):
            return (30, 30, 30, 255)
        pane = (x // 16) + 2 * (y // 16)
        c = [(200, 40, 40), (40, 90, 200), (230, 200, 40), (40, 170, 80)][pane]
        return c + (150,)
    png(os.path.join(out, "stained_glass.png"), 32, 32, stained, True)
    print("wrote 6 tiles to %s" % out)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "port/tools/fixtures/packs/emod_example/textures")
