#!/usr/bin/env python3
# U.0: rasterise the Lucide icons the GL UI uses into a white-on-transparent atlas.
#
#   python3 -I port/tools/ui-icons.py            (from the repo root)
#
# Reads the path data from port/tools/eden-icons.js (the retired web UI's icon module, kept as the
# source of truth), strokes it at 2.4 / 24 with round caps and joins at 8x supersampling, and writes
#   media/ui/ui_icons.png   256x256, a 4x4 grid of 64px cells (white on transparent)
#   Classes/UIIcons.h       enum UIIcon -> cell index + UIIconCell() -> (col,row)
# Lucide is ISC; its notice sits next to the atlas (media/ui/LUCIDE-LICENSE.txt).
# Deterministic: running it twice gives identical bytes.
import math, os, re, sys
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ICONS = ["wrench", "volume-2", "gamepad-2", "keyboard", "monitor", "flask-conical",
         "hard-drive", "settings", "square", "mountain", "play", "info", "trees"]
FILLED = {"play"}
CELL, SS, GRID, STROKE = 64, 8, 4, 2.4

def tokens(d):
    return re.findall(r"[A-Za-z]|-?(?:\d+\.?\d*|\.\d+)(?:e-?\d+)?", d)

def arc_pts(x1, y1, rx, ry, phi, fa, fs, x2, y2, n=24):
    if rx == 0 or ry == 0 or (x1 == x2 and y1 == y2):
        return [(x2, y2)]
    rx, ry = abs(rx), abs(ry)
    c, s = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = c * dx + s * dy, -s * dx + c * dy
    lam = x1p**2 / rx**2 + y1p**2 / ry**2
    if lam > 1:
        rx *= math.sqrt(lam); ry *= math.sqrt(lam)
    num = rx**2 * ry**2 - rx**2 * y1p**2 - ry**2 * x1p**2
    den = rx**2 * y1p**2 + ry**2 * x1p**2
    k = math.sqrt(max(0, num / den)) * (-1 if fa == fs else 1)
    cxp, cyp = k * rx * y1p / ry, -k * ry * x1p / rx
    cx, cy = c * cxp - s * cyp + (x1 + x2) / 2, s * cxp + c * cyp + (y1 + y2) / 2
    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a
    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not fs and dt > 0: dt -= 2 * math.pi
    if fs and dt < 0: dt += 2 * math.pi
    out = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        px, py = rx * math.cos(t), ry * math.sin(t)
        out.append((c * px - s * py + cx, s * px + c * py + cy))
    return out

def bez(p0, ps, n=16):
    out = []
    for i in range(1, n + 1):
        t = i / n; u = 1 - t
        pts = [p0] + ps
        if len(pts) == 4:
            x = u**3*pts[0][0] + 3*u*u*t*pts[1][0] + 3*u*t*t*pts[2][0] + t**3*pts[3][0]
            y = u**3*pts[0][1] + 3*u*u*t*pts[1][1] + 3*u*t*t*pts[2][1] + t**3*pts[3][1]
        else:
            x = u*u*pts[0][0] + 2*u*t*pts[1][0] + t*t*pts[2][0]
            y = u*u*pts[0][1] + 2*u*t*pts[1][1] + t*t*pts[2][1]
        out.append((x, y))
    return out

def flatten(d):
    """-> list of (points, closed). Supports M L H V C S Q A Z, absolute and relative."""
    t = tokens(d); i = 0
    subs = []; cur = None
    x = y = sx = sy = 0.0
    cmd = None; lastc = None; lastq = None
    def num():
        nonlocal i
        v = float(t[i]); i += 1; return v
    while i < len(t):
        if re.match(r"[A-Za-z]", t[i]):
            cmd = t[i]; i += 1
            if cmd in "Zz":
                if cur: subs.append((cur, True))
                cur = None; x, y = sx, sy; continue
        rel = cmd.islower(); C = cmd.upper()
        ox, oy = (x, y) if rel else (0, 0)
        if C == "M":
            x, y = ox + num(), oy + num(); sx, sy = x, y
            if cur: subs.append((cur, False))
            cur = [(x, y)]; cmd = "l" if rel else "L"; lastc = lastq = None; continue
        if cur is None: cur = [(x, y)]
        if C == "L":
            x, y = ox + num(), oy + num(); cur.append((x, y)); lastc = lastq = None
        elif C == "H":
            x = ox + num(); cur.append((x, y)); lastc = lastq = None
        elif C == "V":
            y = oy + num(); cur.append((x, y)); lastc = lastq = None
        elif C == "C":
            a = (ox + num(), oy + num()); b = (ox + num(), oy + num()); e = (ox + num(), oy + num())
            cur += bez((x, y), [a, b, e]); x, y = e; lastc = b; lastq = None
        elif C == "S":
            a = (2*x - lastc[0], 2*y - lastc[1]) if lastc else (x, y)
            b = (ox + num(), oy + num()); e = (ox + num(), oy + num())
            cur += bez((x, y), [a, b, e]); x, y = e; lastc = b; lastq = None
        elif C == "Q":
            a = (ox + num(), oy + num()); e = (ox + num(), oy + num())
            cur += bez((x, y), [a, e]); x, y = e; lastq = a; lastc = None
        elif C == "A":
            rx, ry, phi, fa, fs = num(), num(), num(), int(num()), int(num())
            e = (ox + num(), oy + num())
            cur += arc_pts(x, y, rx, ry, phi, fa, fs, *e); x, y = e; lastc = lastq = None
        else:
            sys.exit(f"unsupported path command {cmd!r} in {d[:40]}")
    if cur: subs.append((cur, False))
    return subs

def load_paths():
    js = open(os.path.join(ROOT, "port/tools/eden-icons.js")).read()
    out = {}
    for name in ICONS:
        m = re.search(r"(?:'%s'|\b%s)\s*:\s*'([^']+)'" % (re.escape(name), re.escape(name)), js)
        if not m: sys.exit(f"icon {name!r} not in eden-icons.js")
        out[name] = m.group(1).split("|")
    return out

def draw_icon(paths, filled):
    S = CELL * SS; k = S / 24.0
    im = Image.new("L", (S, S), 0); dr = ImageDraw.Draw(im)
    w = STROKE * k; r = w / 2
    for seg in paths:
        for pts, closed in flatten(seg):
            p = [(px * k, py * k) for px, py in pts]
            if filled:
                dr.polygon(p, fill=255); continue
            if closed: p = p + [p[0]]
            if len(p) == 1: p = p * 2
            dr.line(p, fill=255, width=round(w), joint="curve")
            for q in p: dr.ellipse([q[0]-r, q[1]-r, q[0]+r, q[1]+r], fill=255)
    return im.resize((CELL, CELL), Image.LANCZOS)

def main():
    paths = load_paths()
    atlas = Image.new("RGBA", (CELL * GRID, CELL * GRID), (255, 255, 255, 0))
    for n, name in enumerate(ICONS):
        a = draw_icon(paths[name], name in FILLED)
        cell = Image.new("RGBA", (CELL, CELL), (255, 255, 255, 0)); cell.putalpha(a)
        atlas.paste(cell, ((n % GRID) * CELL, (n // GRID) * CELL))
    atlas.save(os.path.join(ROOT, "media/ui/ui_icons.png"), optimize=False)
    ids = [re.sub(r"[^A-Za-z0-9]", "_", n).upper() for n in ICONS]
    h = ["// GENERATED by port/tools/ui-icons.py -- do not edit. Cells of media/ui/ui_icons.png",
         "// (256x256, 4x4 grid of 64px cells, white on transparent; tint at draw time).",
         "// Glyphs: Lucide (ISC), see media/ui/LUCIDE-LICENSE.txt and docs/third-party.md.",
         "#ifndef UIIcons_h", "#define UIIcons_h", "",
         "enum UIIcon {"]
    h += [f"    UIICON_{i} = {n}," for n, i in enumerate(ids)]
    h += ["    UIICON_COUNT = %d," % len(ids), "    UIICON_FALLBACK = UIICON_SETTINGS",
          "};", "", "#define UIICON_CELL_PX 64", "#define UIICON_GRID 4",
          "static inline int UIIconCol(int icon) { return icon % UIICON_GRID; }",
          "static inline int UIIconRow(int icon) { return icon / UIICON_GRID; }", "",
          "#endif", ""]
    open(os.path.join(ROOT, "Classes/UIIcons.h"), "w").write("\n".join(h))

main()
