#!/usr/bin/env python3
"""eden-format-measure.py -- READ-ONLY measurement of a `.eden` world for the `.emod` Phase 0 study
(WORKING/ROADMAP.md Stage F, row F.1). Never writes to the world; scratch output goes to $TMPDIR only
for the optional whole-file xz/7z runs.

Parsing mirrors tools/eden-convert.js (u64 chunk_offset, column span derived from the gap to the NEXT
column, creature-slot count derived from the file, contiguous 0xffffffff sign trailer at the end of the
directory). Dependencies (not stdlib): `pip install zstandard lz4`. zlib is stdlib. xz/7z are CLI.

  python3 eden-format-measure.py WORLD.eden [--stride N] [--whole-file] [--default-map Eden.eden]
                                           [--time-sample N] [--json out.json]
  python3 eden-format-measure.py --corrupt-gate          # prove the round-trip check can FAIL

--stride N     measure every Nth column (by file offset) and extrapolate sizes; occupancy/default-map
               use the same sample. Default 1 (every column).
--whole-file   also run `xz -T0 -6` and `7z a -mx=5` over the entire file (slow on multi-GB worlds).
--default-map  RLE Eden.eden to compare against (only used when the header seed == 333333).
--time-sample  columns used for per-column encode/decode timing (default 300, spread evenly,
               skipping nothing: air-heavy and dense columns both get timed).
"""
import argparse, json, os, struct, subprocess, sys, tempfile, time, zlib

try:
    import zstandard
    import lz4.block
except ImportError:
    zstandard = None

HEADER = 192
BAND = 8192            # 4096 types + 4096 paint
DIR_ROW = 16
ENT = 60
SEED_DEFAULT = 333333


# ------------------------------------------------------------------ parsing
def read_world(path):
    f = open(path, 'rb')
    size = os.fstat(f.fileno()).st_size
    h = f.read(HEADER)
    if len(h) != HEADER:
        raise SystemExit('shorter than a 192-byte header')
    hd = dict(seed=struct.unpack_from('<i', h, 0)[0], version=struct.unpack_from('<i', h, 92)[0],
              dir_off=struct.unpack_from('<Q', h, 32)[0])
    nm = h[40:90]
    hd['name'] = nm.split(b'\0')[0].decode('utf8', 'replace')
    if not (1 <= hd['version'] <= 1000):
        raise SystemExit('legacy 1.x file (version %d): not handled' % hd['version'])
    if hd['version'] >= 5:
        bands, slots_default = 16, 400
    else:
        bands, slots_default = 4, 200
    col = bands * BAND
    if not (HEADER <= hd['dir_off'] <= size):
        raise SystemExit('directory offset outside file')
    f.seek(hd['dir_off'])
    raw = f.read(size - hd['dir_off'])
    rows = [struct.unpack_from('<iiQ', raw, i * DIR_ROW) for i in range(len(raw) // DIR_ROW)]
    # contiguous gate-failing run at the END = sign trailer (kept verbatim by the engine)
    keep = len(rows)
    while keep > 0 and not (0 <= rows[keep - 1][0] < 32768 and 0 <= rows[keep - 1][1] < 32768):
        keep -= 1
    trailer_rows = len(rows) - keep
    ents = [(x, z, o) for (x, z, o) in rows[:keep] if (x << 15 | z) != 0]
    ents.sort(key=lambda e: e[2])
    creature_bytes = 0
    if hd['version'] >= 3:
        if ents:
            gap = hd['dir_off'] - (ents[-1][2] + col)
            creature_bytes = gap if gap >= 0 and gap % ENT == 0 else slots_default * ENT
            creature_suspect = not (gap >= 0 and gap % ENT == 0)
        else:
            creature_bytes = slots_default * ENT
            creature_suspect = False
    else:
        creature_suspect = False
    block_end = hd['dir_off'] - creature_bytes
    spans = []
    for i, (x, z, o) in enumerate(ents):
        nxt = ents[i + 1][2] if i + 1 < len(ents) else block_end
        spans.append(max(0, min(col, nxt - o)))
    hd.update(bands=bands, col=col, size=size, columns=len(ents), dir_rows=len(rows),
              trailer_rows=trailer_rows, trailer_bytes=trailer_rows * DIR_ROW,
              creature_slots=creature_bytes // ENT, creature_suspect=creature_suspect,
              short_spans=sum(1 for s in spans if s < col),
              referenced=sum(spans), first_col=ents[0][2] if ents else 0)
    return f, hd, ents, spans


def read_col(f, hd, off, span):
    f.seek(off)
    b = f.read(span)
    return b + bytes(hd['col'] - len(b))   # zero-pad: a short span reads as air


# ------------------------------------------------------------------ RLE default map
def rle_default(path):
    f = open(path, 'rb')
    size = os.fstat(f.fileno()).st_size
    h = f.read(HEADER)
    dir_off = struct.unpack_from('<Q', h, 32)[0]
    f.seek(dir_off)
    raw = f.read(size - dir_off)
    idx = {}
    for i in range(len(raw) // DIR_ROW):
        x, z, o = struct.unpack_from('<iiQ', raw, i * DIR_ROW)
        if 0 <= x < 32768 and 0 <= z < 32768:
            idx[(x, z)] = o
    return f, idx


def rle_column(f, off, bands=4):
    """Return the RAW (un-transposed) 4-band column the engine would see, or None on a bad record."""
    f.seek(off)
    out = bytearray(bands * BAND)
    for cy in range(bands):
        ln = f.read(2)
        if len(ln) < 2:
            return None
        n = ln[0] * 256 + ln[1] - 2
        buf = f.read(n)
        t = bytearray(4096); c = bytearray(4096); k = 0
        for i in range(0, len(buf) - 2, 3):
            ty, co, cnt = buf[i], buf[i + 1], buf[i + 2]
            if k + cnt > 4096:
                cnt = 4096 - k
            t[k:k + cnt] = bytes([ty]) * cnt
            c[k:k + cnt] = bytes([co]) * cnt
            k += cnt
            if k >= 4096:
                break
        if k != 4096:
            return None
        base = cy * BAND
        # un-transpose: out[CC(x,z,y)] = t[CC(y,z,x)], CC(a,b,c)=a*256+b*16+c
        for x in range(16):
            for z in range(16):
                s = z * 16
                for y in range(16):
                    out[base + x * 256 + s + y] = t[y * 256 + s + x]
                    out[base + 4096 + x * 256 + s + y] = c[y * 256 + s + x]
    return bytes(out)


# ------------------------------------------------------------------ codecs
def codecs():
    z = {l: zstandard.ZstdCompressor(level=l) for l in (1, 3, 9, 19)}
    d = zstandard.ZstdDecompressor()
    return z, d


def elide(col, bands):
    """band-elision: 16-bit mask + only the non-all-zero 8192-byte bands. Returns (mask, payload)."""
    mask = 0; parts = []
    for b in range(bands):
        seg = col[b * BAND:(b + 1) * BAND]
        if seg.count(0) != BAND:
            mask |= 1 << b
            parts.append(seg)
    return mask, b''.join(parts)


def unelide(mask, payload, bands):
    out = bytearray(bands * BAND); p = 0
    for b in range(bands):
        if mask >> b & 1:
            out[b * BAND:(b + 1) * BAND] = payload[p:p + BAND]; p += BAND
    return bytes(out)


def roundtrip_ok(col, bands, z3, d):
    """Encode with every codec we project, decode, compare to the raw bytes. Returns list of failures."""
    bad = []
    if d.decompress(z3.compress(col), max_output_size=len(col)) != col: bad.append('zstd3')
    if zlib.decompress(zlib.compress(col, 6)) != col: bad.append('zlib6')
    if lz4.block.decompress(lz4.block.compress(col, store_size=False), uncompressed_size=len(col)) != col: bad.append('lz4')
    m, p = elide(col, bands)
    if unelide(m, d.decompress(z3.compress(p), max_output_size=max(len(p), 1)) if p else b'', bands) != col: bad.append('elide+zstd3')
    return bad


def corrupt_gate():
    """The F.1 gate: run the round-trip check on a corrupted byte and WATCH IT FAIL."""
    z3 = zstandard.ZstdCompressor(level=3); d = zstandard.ZstdDecompressor()
    import random
    rnd = random.Random(1)
    col = bytearray(16 * BAND)
    for i in range(0, 3 * BAND):
        col[i] = rnd.choice((0, 0, 0, 1, 2, 3))
    col = bytes(col)
    assert roundtrip_ok(col, 16, z3, d) == [], 'clean round trip must pass first'
    print('clean column: round-trip OK for zstd3, zlib6, lz4, elide+zstd3')
    # corrupt one byte of the DECODED output of each path and re-compare, plus one corrupted blob
    bad = bytearray(col); bad[5000] ^= 0x01; bad = bytes(bad)
    caught = []
    if d.decompress(z3.compress(col)) == bad: caught.append('zstd3 NOT caught')
    if zlib.decompress(zlib.compress(col, 6)) == bad: caught.append('zlib6 NOT caught')
    m, p = elide(col, 16)
    if unelide(m, p, 16) == bad: caught.append('elide NOT caught')
    blob = bytearray(z3.compress(col)); blob[len(blob) // 2] ^= 0xff
    try:
        r = d.decompress(bytes(blob), max_output_size=len(col))
        res = 'mismatch' if r != col else 'NOT caught'
    except Exception as e:
        res = 'decoder error (%s)' % type(e).__name__
    print('corrupted compressed blob (1 byte flipped): %s' % res)
    print('corrupted expected-bytes (1 bit flipped) vs each codec:', 'all detected' if not caught else caught)
    if caught or res == 'NOT caught':
        print('GATE FAILED: the round-trip check does not detect corruption'); sys.exit(2)
    print('GATE PASSED: the round-trip check fails when a byte is corrupted')


# ------------------------------------------------------------------ main measurement
def pct(a, b):
    return '%.1f%%' % (100.0 * a / b) if b else 'n/a'


def mb(n):
    return '%.1f MB' % (n / 1048576.0)


def measure(a):
    f, hd, ents, spans = read_world(a.world)
    bands, col = hd['bands'], hd['col']
    if hd['columns'] and hd['short_spans'] == hd['columns']:
        raise SystemExit('every column span is shorter than a raw record: this looks like the RLE-compressed bundled '
                         'Eden.eden, which this tool does not measure as raw columns (it is only read via --default-map)')
    print('=== %s' % a.world)
    print('header: name=%r seed=%d version=%d  => %d bands/column (%d B/column)' % (hd['name'], hd['seed'], hd['version'], bands, col))
    print('file: %s (%d B)  columns: %d  directory rows: %d  trailer rows: %d (%d B)' % (mb(hd['size']), hd['size'], hd['columns'], hd['dir_rows'], hd['trailer_rows'], hd['trailer_bytes']))
    print('creature block: %d slots%s   short spans: %d' % (hd['creature_slots'], ' (SUSPECT: assumed from version)' if hd['creature_suspect'] else ' (derived)', hd['short_spans']))
    live = hd['columns'] * col
    print('live column data (columns x record size): %s; file bytes before the creature block: %s -> %s are dead/overwritten records' % (
        mb(live), mb(hd['dir_off'] - hd['creature_slots'] * ENT - hd['first_col']), mb(max(0, hd['dir_off'] - hd['creature_slots'] * ENT - hd['first_col'] - live))))
    if zstandard is None:
        raise SystemExit('pip install zstandard lz4')
    zs, d = codecs()
    stride = max(1, a.stride)
    sel = list(range(0, len(ents), stride))
    n = len(sel)
    tot = dict(raw=0, zstd1=0, zstd3=0, zstd9=0, zstd19=0, zlib6=0, lz4=0, elide=0, elide_z3=0)
    mask_hist = [0] * (bands + 1)    # highest occupied band + 1 (0 = fully empty)
    air_by_band = [0] * bands
    empty_above3 = 0
    n_dense_nonair_bands = 0
    paint_in_air = 0
    rt_fail = 0
    t0 = time.time()
    default = None; dmatch = dmiss = dnone = 0
    dmatch_hi_air = 0
    if hd['seed'] == SEED_DEFAULT and a.default_map and os.path.exists(a.default_map):
        default = rle_default(a.default_map)
    for k, i in enumerate(sel):
        x, z, off = ents[i]
        c = read_col(f, hd, off, spans[i])
        tot['raw'] += col
        for L in (1, 3, 9, 19):
            tot['zstd%d' % L] += len(zs[L].compress(c))
        tot['zlib6'] += len(zlib.compress(c, 6))
        tot['lz4'] += len(lz4.block.compress(c, store_size=False))
        m, p = elide(c, bands)
        tot['elide'] += 2 + len(p)
        ez = zs[3].compress(p) if p else b''
        tot['elide_z3'] += 2 + len(ez)
        top = 0
        for b in range(bands):
            if m >> b & 1: top = b + 1
            else:
                air_by_band[b] += 1
        # "air band" for elision = whole 8192 B zero; also count bands whose TYPES are all air but paint is not
        for b in range(bands):
            if not (m >> b & 1): continue
            if c[b * BAND:b * BAND + 4096].count(0) == 4096: paint_in_air += 1
        mask_hist[top] += 1
        if bands == 16 and top <= 4: empty_above3 += 1
        if default is not None:
            df, didx = default
            o = didx.get((x, z))
            if o is None:
                dnone += 1
            else:
                dc = rle_column(df, o)
                if dc is None: dnone += 1
                elif c[:4 * BAND] == dc and (bands == 4 or c[4 * BAND:].count(0) == len(c) - 4 * BAND):
                    dmatch += 1
                else:
                    dmiss += 1
        if roundtrip_ok(c, bands, zs[3], d):
            rt_fail += 1
        if (k + 1) % 2000 == 0:
            print('  ... %d/%d columns (%.0fs)' % (k + 1, n, time.time() - t0), file=sys.stderr)
    scale = len(ents) / float(n)
    print('\n-- projected per-column compressed totals (%d of %d columns measured, stride %d; extrapolated x%.2f)' % (n, len(ents), stride, scale))
    print('%-22s %14s %12s %9s %9s' % ('codec', 'projected', 'B/column', 'of raw', 'file*'))
    fixed = hd['size'] - live
    for key, label in (('raw', 'raw (today)'), ('lz4', 'lz4'), ('zlib6', 'zlib-6'), ('zstd1', 'zstd-1'), ('zstd3', 'zstd-3'),
                       ('zstd9', 'zstd-9'), ('zstd19', 'zstd-19'), ('elide', 'band-elision only'), ('elide_z3', 'elision + zstd-3')):
        proj = tot[key] * scale
        print('%-22s %14s %12.0f %9s %9s' % (label, mb(proj), tot[key] / n, pct(tot[key], tot['raw']), mb(proj + max(0, fixed))))
    print('  (*file = projection + non-column bytes of the current file [header, creatures, directory, dead records]; the dead-record'
          ' share is NOT carried over by a compacting container, so treat as an upper bound)')
    print('\n-- band occupancy over %d sampled columns' % n)
    print('highest occupied band (0 = column entirely air):')
    for t in range(bands + 1):
        if mask_hist[t]: print('  top=%2d  %7d  %s' % (t, mask_hist[t], pct(mask_hist[t], n)))
    print('% all-air (8192 B all zero) per band: ' + ' '.join('b%d=%s' % (b, pct(air_by_band[b], n)) for b in range(bands)))
    print('all-air bands overall: %s of %d' % (pct(sum(air_by_band), n * bands), n * bands))
    if bands == 16:
        print('columns with bands 4..15 all air (empty above band 3): %s' % pct(empty_above3, n))
    print('occupied bands whose TYPE plane is all air but paint is nonzero: %d (elision keeps these)' % paint_in_air)
    if default is not None:
        print('\n-- vs bundled default map (seed 333333; %s)' % ('bands 0-3 equal AND 4..15 air' if bands == 16 else 'all 4 bands equal'))
        print('identical: %d (%s)   differ: %d (%s)   no default column: %d (%s)' % (dmatch, pct(dmatch, n), dmiss, pct(dmiss, n), dnone, pct(dnone, n)))
    else:
        print('\n-- vs bundled default map: n/a (header seed %d != %d, or --default-map not given)' % (hd['seed'], SEED_DEFAULT))
    print('\n-- round trip (zstd-3, zlib-6, lz4, elision+zstd-3 decode == raw bytes) over ALL %d sampled columns: %s' % (n, 'PASS' if rt_fail == 0 else 'FAIL x%d' % rt_fail))
    # ---- timing, single-threaded, on an evenly spread subset
    ts = max(1, len(ents) // max(1, a.time_sample))
    tsel = list(range(0, len(ents), ts))[:a.time_sample]
    cols = [read_col(f, hd, ents[i][2], spans[i]) for i in tsel]
    z3 = zs[3]
    pre = [(z3.compress(c), zlib.compress(c, 6)) for c in cols]
    def bench(fn):
        t = time.perf_counter()
        for x in fn(): pass
        return (time.perf_counter() - t) / len(cols) * 1e6
    e_z = bench(lambda: (z3.compress(c) for c in cols))
    e_l = bench(lambda: (zlib.compress(c, 6) for c in cols))
    d_z = bench(lambda: (d.decompress(p[0], max_output_size=col) for p in pre))
    d_l = bench(lambda: (zlib.decompress(p[1]) for p in pre))
    print('\n-- per-column time on this Mac (C-backed zstandard/zlib, %d columns evenly spread; NOT an iPad figure), microseconds:' % len(cols))
    print('zstd-3 encode %.0f   decode %.0f      zlib-6 encode %.0f   decode %.0f' % (e_z, d_z, e_l, d_l))
    print('(for scale: reading one raw column record = %d B)' % col)
    res = dict(header=hd, sampled=n, totals=tot, scale=scale, mask_hist=mask_hist, air_by_band=air_by_band,
               empty_above3=empty_above3, default=dict(match=dmatch, differ=dmiss, none=dnone),
               roundtrip_failures=rt_fail, time_us=dict(zstd3_enc=e_z, zstd3_dec=d_z, zlib6_enc=e_l, zlib6_dec=d_l))
    if a.whole_file:
        print('\n-- whole-file compressors (what a player gets zipping the file; includes dead records)')
        for label, cmd in (('xz -T0 -6', ['xz', '-T0', '-6', '-c', a.world]),):
            t = time.time(); sz = 0
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
            while True:
                b = p.stdout.read(1 << 20)
                if not b: break
                sz += len(b)
            p.wait()
            print('%-12s %s  (%s of file)  %.0fs' % (label, mb(sz), pct(sz, hd['size']), time.time() - t)); res[label] = sz
        t = time.time()
        with tempfile.TemporaryDirectory() as td:
            out = os.path.join(td, 'x.7z')
            subprocess.run(['7z', 'a', '-mx=5', '-bd', '-bso0', out, a.world], check=True)
            sz = os.path.getsize(out)
        print('%-12s %s  (%s of file)  %.0fs' % ('7z -mx=5', mb(sz), pct(sz, hd['size']), time.time() - t)); res['7z'] = sz
    if a.json:
        json.dump(res, open(a.json, 'w'), indent=1, default=str)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('world', nargs='?')
    ap.add_argument('--stride', type=int, default=1)
    ap.add_argument('--whole-file', action='store_true')
    ap.add_argument('--default-map', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'Eden.eden'))
    ap.add_argument('--time-sample', type=int, default=300)
    ap.add_argument('--json')
    ap.add_argument('--corrupt-gate', action='store_true')
    a = ap.parse_args()
    if a.corrupt_gate:
        return corrupt_gate()
    if not a.world:
        ap.error('world required')
    measure(a)


if __name__ == '__main__':
    main()
