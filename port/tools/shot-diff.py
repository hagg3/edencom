#!/usr/bin/env python3
"""shot-diff.py — compare BMP captures between two builds (G.5b-0, renderer plan §9.4 / R10).

    shot-diff.py REF NEW [--out DIR] [--tol N] [--mask X,Y,W,H ...] [--quiet]

REF and NEW are two BMP files, or two directories (every *.bmp in REF is paired with the same
name in NEW; --scene-shot and --shot write 24-bit BI_RGB BMPs). Per image it reports `identical`,
or the maximum per-channel difference, the number of pixels over --tol (default 0), their share,
and the bounding box of the differing pixels; with --out it writes DIR/<name>-diff.bmp (differing
pixels red over a dimmed copy of NEW). --mask (repeatable, in image pixels, origin top-left)
excludes a region from the comparison: the clock-driven parts of a frame, listed in the results doc.

Exit status: 0 when every pair is identical after masking (or within --tol), 1 otherwise, 2 on a
usage or file error. Standard library only.
"""
import os
import struct
import sys


def read_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError("%s: not a BMP" % path)
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    comp = struct.unpack_from("<I", data, 30)[0]
    if bpp not in (24, 32) or comp != 0:
        raise ValueError("%s: only uncompressed 24/32-bit BMPs (got %d bpp, compression %d)" % (path, bpp, comp))
    bottom_up = h > 0
    h = abs(h)
    stride = ((w * bpp // 8) + 3) & ~3
    px = bytearray(w * h * 3)   # RGB, rows top-down
    step = bpp // 8
    for row in range(h):
        src = off + (h - 1 - row if bottom_up else row) * stride
        line = data[src:src + w * step]
        o = row * w * 3
        for x in range(w):
            b, g, r = line[x * step], line[x * step + 1], line[x * step + 2]
            px[o + x * 3] = r
            px[o + x * 3 + 1] = g
            px[o + x * 3 + 2] = b
    return w, h, px


def write_bmp(path, w, h, px):
    stride = (w * 3 + 3) & ~3
    out = bytearray(54 + stride * h)
    struct.pack_into("<2sIHHI", out, 0, b"BM", len(out), 0, 0, 54)
    struct.pack_into("<IiiHHIIiiII", out, 14, 40, w, h, 1, 24, 0, stride * h, 2835, 2835, 0, 0)
    for row in range(h):
        dst = 54 + (h - 1 - row) * stride
        o = row * w * 3
        for x in range(w):
            r, g, b = px[o + x * 3], px[o + x * 3 + 1], px[o + x * 3 + 2]
            out[dst + x * 3] = b
            out[dst + x * 3 + 1] = g
            out[dst + x * 3 + 2] = r
    with open(path, "wb") as f:
        f.write(out)


def compare(ref, new, tol, masks, out_path):
    wa, ha, a = read_bmp(ref)
    wb, hb, b = read_bmp(new)
    if (wa, ha) != (wb, hb):
        return {"size": "%dx%d vs %dx%d" % (wa, ha, wb, hb)}
    if a == b:
        return None
    masked = bytearray(wa * ha)
    for (mx, my, mw, mh) in masks:
        for y in range(max(0, my), min(ha, my + mh)):
            for x in range(max(0, mx), min(wa, mx + mw)):
                masked[y * wa + x] = 1
    maxd, over, n = 0, 0, 0
    x0, y0, x1, y1 = wa, ha, -1, -1
    diff = bytearray(len(b)) if out_path else None
    for i in range(wa * ha):
        j = i * 3
        d = max(abs(a[j] - b[j]), abs(a[j + 1] - b[j + 1]), abs(a[j + 2] - b[j + 2]))
        if diff is not None:
            if d and not masked[i]:
                diff[j], diff[j + 1], diff[j + 2] = 255, 0, 0
            else:
                diff[j], diff[j + 1], diff[j + 2] = b[j] // 3, b[j + 1] // 3, b[j + 2] // 3
        if not d or masked[i]:
            continue
        n += 1
        maxd = max(maxd, d)
        if d > tol:
            over += 1
        y, x = divmod(i, wa)
        x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
    if n == 0:
        return None
    if diff is not None:
        write_bmp(out_path, wa, ha, diff)
    return {"max": maxd, "over": over, "pixels": n, "share": 100.0 * over / (wa * ha),
            "box": (x0, y0, x1 - x0 + 1, y1 - y0 + 1)}


def main(argv):
    args, tol, masks, out, quiet = [], 0, [], None, False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--tol":
            i += 1; tol = int(argv[i])
        elif a.startswith("--tol="):
            tol = int(a[6:])
        elif a == "--out":
            i += 1; out = argv[i]
        elif a.startswith("--out="):
            out = a[6:]
        elif a == "--mask" or a.startswith("--mask="):
            v = a[7:] if a.startswith("--mask=") else argv[i + 1]
            if a == "--mask":
                i += 1
            masks.append(tuple(int(t) for t in v.split(",")))
        elif a == "--quiet":
            quiet = True
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            args.append(a)
        i += 1
    if len(args) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    ref, new = args
    if os.path.isdir(ref):
        names = sorted(n for n in os.listdir(ref) if n.lower().endswith(".bmp"))
        pairs = [(n, os.path.join(ref, n), os.path.join(new, n)) for n in names]
    else:
        pairs = [(os.path.basename(new), ref, new)]
    if out:
        os.makedirs(out, exist_ok=True)
    bad = 0
    for name, ra, nb in pairs:
        if not os.path.exists(nb):
            print("%-40s MISSING in NEW" % name)
            bad += 1
            continue
        try:
            r = compare(ra, nb, tol, masks, os.path.join(out, name[:-4] + "-diff.bmp") if out else None)
        except (OSError, ValueError) as e:
            print("%-40s ERROR %s" % (name, e))
            return 2
        if r is None:
            if not quiet:
                print("%-40s identical" % name)
        elif "size" in r:
            print("%-40s SIZE %s" % (name, r["size"]))
            bad += 1
        else:
            within = r["over"] == 0
            print("%-40s %s max %d, %d px differ (%d over tol %d, %.3f%%), box x%d y%d %dx%d" %
                  (name, "within-tol" if within else "DIFFERS", r["max"], r["pixels"], r["over"], tol,
                   r["share"], r["box"][0], r["box"][1], r["box"][2], r["box"][3]))
            if not within:
                bad += 1
    print("shot-diff: %d of %d differ" % (bad, len(pairs)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
