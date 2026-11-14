#!/usr/bin/env python3
"""pack.py -- reference tool for Emod content packs (K.2-3, WORKING/data-driven-content-plan-2026-11-01.md).

The normative schema is docs/content-packs.md; where this file and the doc disagree, the doc wins and
this file has a bug. Independent of the engine on purpose: `compile` is what the engine's loader
(Classes/ContentPack.cpp) is checked against, so it must not share that code's mistakes. Its JSON
reader mirrors Classes/Json.cpp (same dialect, same byte offsets, same messages) and its validator
prints the same lines as the engine, so a fixture's expected error is checked by both.

  python3 pack.py validate PACK_DIR [--stock]       exit 0 valid, 1 refused or a block skipped (one line each);
                                                    the stock rules for a folder named `eden` (or --stock),
                                                    the mod rules otherwise (K.2-4)
  python3 pack.py compile  PACK_DIR                 the `--content-dump` text the stock pack compiles to
  python3 pack.py compile  PACK_DIR --check DUMP    ... and diff it against a dump (fixture or a binary's log)
  python3 pack.py compile  MOD_DIR... [--check LOG] the `## mods` section `eden_native --content-dump
                                                    --packs=MOD_DIR...` prints (a pack, or a folder of packs)
  python3 pack.py extract  DUMP -o PACK.json        one-off (K.2-3): the stock pack from the pinned dump
  python3 pack.py fixtures DIR                      each DIR/<case>/ must be refused with exactly the lines of
                                                    its expect.txt, in order (the engine's --pack-selftest
                                                    reads the same set)

A fixture folder named `mod-*` is checked with the mod rules (K.2-4) and its expect.txt also carries
the verdict (`= refused` or `= blocks N`). Python 3.8+, stdlib only.
"""
import math
import os
import re
import sys

# ---------------------------------------------------------------------------------------------
# JSON (Classes/Json.cpp): RFC 8259 + `//` line comments, no trailing commas, no duplicate keys,
# 1 MiB, depth 32. A value is (type, payload, offset); objects keep document order.

MAX_BYTES = 1 << 20
MAX_DEPTH = 32


class JsonError(Exception):
    def __init__(self, offset, message):
        Exception.__init__(self, message)
        self.offset = offset
        self.message = message


class V(object):
    __slots__ = ("t", "v", "off")

    def __init__(self, t, v, off):
        self.t, self.v, self.off = t, v, off

    def get(self, key):
        if self.t != "object":
            return None
        for k, val in self.v:
            if k == key:
                return val
        return None


class _Reader(object):
    def __init__(self, data):
        self.p, self.n, self.i = data, len(data), 0

    def fail(self, msg, at=None):
        if at is not None:
            self.i = at
        raise JsonError(self.i, msg)

    def skip(self):
        p, n = self.p, self.n
        while True:
            while self.i < n and p[self.i] in b" \t\n\r":
                self.i += 1
            if self.i < n and p[self.i] == 0x2F:
                if self.i + 1 < n and p[self.i + 1] == 0x2F:
                    while self.i < n and p[self.i] != 0x0A:
                        self.i += 1
                    continue
                self.fail("'/' that does not start a // comment")
            return

    def literal(self, word):
        if self.p[self.i:self.i + len(word)] != word:
            self.fail("unexpected character")
        self.i += len(word)

    def hex4(self):
        if self.n - self.i < 4:
            self.fail("truncated \\u escape")
        v = 0
        for k in range(4):
            c = chr(self.p[self.i + k])
            if c not in "0123456789abcdefABCDEF":
                self.i += k
                self.fail("bad hex digit in \\u escape")
            v = v * 16 + int(c, 16)
        self.i += 4
        return v

    def utf8(self, out):
        p, i = self.p, self.i
        c = p[i]
        if 0xC2 <= c <= 0xDF:
            ln, cp, mn = 2, c & 0x1F, 0x80
        elif 0xE0 <= c <= 0xEF:
            ln, cp, mn = 3, c & 0x0F, 0x800
        elif 0xF0 <= c <= 0xF4:
            ln, cp, mn = 4, c & 0x07, 0x10000
        else:
            self.fail("invalid UTF-8")
        if self.n - i < ln:
            self.fail("invalid UTF-8")
        for k in range(1, ln):
            d = p[i + k]
            if d & 0xC0 != 0x80:
                self.fail("invalid UTF-8")
            cp = (cp << 6) | (d & 0x3F)
        if cp < mn or cp > 0x10FFFF or 0xD800 <= cp <= 0xDFFF:
            self.fail("invalid UTF-8")
        out.extend(p[i:i + ln])
        self.i += ln

    def string(self):
        self.i += 1
        out = bytearray()
        p, n = self.p, self.n
        while True:
            if self.i >= n:
                self.fail("unterminated string")
            c = p[self.i]
            if c == 0x22:
                self.i += 1
                return out.decode("utf-8")
            if c < 0x20:
                self.fail("control character in string")
            if c >= 0x80:
                self.utf8(out)
                continue
            if c != 0x5C:
                out.append(c)
                self.i += 1
                continue
            esc = self.i
            self.i += 1
            if self.i >= n:
                self.fail("unterminated string")
            e = chr(p[self.i])
            self.i += 1
            simple = {'"': '"', "\\": "\\", "/": "/", "b": "\b", "f": "\f", "n": "\n", "r": "\r", "t": "\t"}
            if e in simple:
                out.extend(simple[e].encode())
            elif e == "u":
                cp = self.hex4()
                if 0xDC00 <= cp <= 0xDFFF:
                    self.fail("unpaired surrogate in \\u escape", esc)
                if 0xD800 <= cp <= 0xDBFF:
                    if self.n - self.i < 2 or p[self.i] != 0x5C or p[self.i + 1] != 0x75:
                        self.fail("unpaired surrogate in \\u escape", esc)
                    self.i += 2
                    lo = self.hex4()
                    if not 0xDC00 <= lo <= 0xDFFF:
                        self.fail("unpaired surrogate in \\u escape", esc)
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
                out.extend(chr(cp).encode("utf-8"))
            else:
                self.fail("bad escape in string", esc)

    def number(self):
        p, n = self.p, self.n
        start = self.i
        digits = b"0123456789"
        if p[self.i] == 0x2D:
            self.i += 1
        if self.i >= n:
            self.fail("bad number")
        if p[self.i] == 0x30:
            self.i += 1
        elif 0x31 <= p[self.i] <= 0x39:
            while self.i < n and p[self.i] in digits:
                self.i += 1
        else:
            self.fail("bad number")
        if self.i < n and p[self.i] == 0x2E:
            self.i += 1
            if self.i >= n or p[self.i] not in digits:
                self.fail("bad number: digit expected after '.'")
            while self.i < n and p[self.i] in digits:
                self.i += 1
        if self.i < n and p[self.i] in b"eE":
            self.i += 1
            if self.i < n and p[self.i] in b"+-":
                self.i += 1
            if self.i >= n or p[self.i] not in digits:
                self.fail("bad number: digit expected in exponent")
            while self.i < n and p[self.i] in digits:
                self.i += 1
        v = float(p[start:self.i].decode())
        if not math.isfinite(v):
            self.fail("number out of range", start)
        return v

    def value(self, depth):
        self.skip()
        if self.i >= self.n:
            self.fail("value expected")
        off = self.i
        c = chr(self.p[self.i])
        if c in "{[":
            if depth >= MAX_DEPTH:
                self.fail("nesting deeper than 32")
            self.i += 1
            is_obj = c == "{"
            close = 0x7D if is_obj else 0x5D
            items = []
            self.skip()
            if self.i < self.n and self.p[self.i] == close:
                self.i += 1
                return V("object" if is_obj else "array", items, off)
            seen = set()  # QA.10: O(1) duplicate check
            while True:
                if is_obj:
                    self.skip()
                    if self.i >= self.n or self.p[self.i] != 0x22:
                        self.fail("object key (a string) expected")
                    key_at = self.i
                    key = self.string()
                    if key in seen:
                        self.fail("duplicate key", key_at)
                    seen.add(key)
                    self.skip()
                    if self.i >= self.n or self.p[self.i] != 0x3A:
                        self.fail("':' expected")
                    self.i += 1
                    items.append((key, self.value(depth + 1)))
                else:
                    items.append(self.value(depth + 1))
                self.skip()
                want = "',' or '}' expected" if is_obj else "',' or ']' expected"
                if self.i >= self.n:
                    self.fail(want)
                if self.p[self.i] == close:
                    self.i += 1
                    return V("object" if is_obj else "array", items, off)
                if self.p[self.i] != 0x2C:
                    self.fail(want)
                self.i += 1
                self.skip()
                if self.i < self.n and self.p[self.i] == close:
                    self.fail("trailing comma")
        if c == '"':
            return V("string", self.string(), off)
        if c == "t":
            self.literal(b"true")
            return V("bool", True, off)
        if c == "f":
            self.literal(b"false")
            return V("bool", False, off)
        if c == "n":
            self.literal(b"null")
            return V("null", None, off)
        if c == "-" or c.isdigit():
            return V("number", self.number(), off)
        self.fail("unexpected character")


def json_parse(data):
    if len(data) > MAX_BYTES:
        raise JsonError(MAX_BYTES, "document larger than 1 MiB")
    r = _Reader(data)
    if data[:3] == b"\xef\xbb\xbf":
        r.i = 3
    v = r.value(0)
    r.skip()
    if r.i != len(data):
        r.fail("text after the value")
    return v


def as_int(v, lo, hi):
    if v is None or v.t != "number" or v.v != math.floor(v.v) or abs(v.v) >= 9007199254740992.0:
        return None
    x = int(v.v)
    return x if lo <= x <= hi else None


# ---------------------------------------------------------------------------------------------
# The vocabulary (docs/content-packs.md; the engine's copy is Classes/ContentPack.cpp).

STOCK_IDS = 128
BLOCK_IDS = 256
ATLAS_TILES = 128
SOUND_SETS_MAX = 64
BLOCKS_MAX = 512
PICKER_PAGE_CELLS = [35, 20]      # Hud.h NUM_DISPLAY_BLOCKS, NUM_PICKER_PAGE2_CELLS
TOOLS = {"tool:sign": 1000, "tool:cmd": 1001}   # Hud.h HUD_TOOL_*

IS_NOTSOLID = 0x2
IS_ATLAS2 = 0x20
IS_LIQUID = 0x40
IS_OBJECT = 0x1000
IS_HARD = 0x8000
IS_MISSING = 1 << 17
IS_CLIMB, IS_BOUNCY, IS_SLIPPERY, IS_BLASTPROOF, IS_UNBREAKABLE, IS_NOSHADOW = [1 << b for b in range(18, 24)]
# "solid" is the absence of IS_NOTSOLID; "paint_art" is blockPaintArt = every site.
TRAITS = {"solid": 0, "hard": IS_HARD, "flammable": 0x1, "transparent": IS_ATLAS2, "climbable": IS_CLIMB,
          "bouncy": IS_BOUNCY, "slippery": IS_SLIPPERY, "blastproof": IS_BLASTPROOF,
          "unbreakable": IS_UNBREAKABLE, "no_shadow": IS_NOSHADOW, "paint_art": 0}
TRAIT_ORDER = ["solid", "hard", "flammable", "transparent", "climbable", "bouncy", "slippery", "blastproof",
               "unbreakable", "no_shadow", "paint_art"]
FLAGS = {"ramp": 0x4, "side": 0x8, "ramp_or_side": 0x10, "liquid": IS_LIQUID, "water": 0x80, "lava": 0x100,
         "colored_special": 0x200, "grass": 0x400, "ice": 0x800, "object": IS_OBJECT, "door": 0x2000,
         "portal": 0x4000, "block_tnt": 0x10000}
FLAG_ORDER = ["ramp", "side", "ramp_or_side", "liquid", "water", "lava", "colored_special", "grass", "ice",
              "object", "door", "portal", "block_tnt"]
KINDS = ["cube", "air", "grass", "tnt", "liquid", "ramp", "side", "firework", "door", "object", "lightbox",
         "portal", "custom", "block_tnt", "bt"]
SITES = ["chunk", "batch", "cursor", "icon", "picker", "particle"]   # BlockTraits.h ART_* bit order
ART_ALL = 63
TNT_GROUPS = ["other", "dirt", "stone", "glass"]                       # BlockTraits.h TRAIT_TNT_*
# Resources.h SOUND_TYPES: the ids a sound set may name (the enum's name minus S_, lowercased).
SOUNDS = {"land_soft": 3, "land_hard": 4, "build_wood": 9, "build_water": 10, "build_stone": 11,
          "build_leaves": 12, "build_lava": 13, "build_glass": 14, "build_dirt": 15, "build_generic": 16,
          "break_wood": 17, "break_water": 18, "break_stone": 19, "break_leaves": 20, "break_lava": 21,
          "break_dirt": 22, "footsteps_hard": 26, "footsteps_soft": 27, "break_glass": 34,
          "break_generic": 36, "build_metal": 47, "build_light": 48, "metal_destroy": 52}
SOUND_NAMES = {v: k for k, v in SOUNDS.items()}

TOP_KEYS = ["format", "id", "name", "version", "tiles", "sound_sets", "blocks", "aliases", "picker_pages"]
TILE_KEYS = ["atlas", "index", "unpainted", "particle_art"]
SET_KEYS = ["build", "break", "land", "step", "tnt"]
BLOCK_KEYS = ["id", "stock_id", "kind", "traits", "flags", "faces", "color", "sounds", "paint_art_sites", "bt",
              "tnt_map"]
FACE_KEYS = ["front", "back", "left", "right", "bottom", "top"]   # blockTypeFaces index order 0..5

RE_PACK_ID = re.compile(r"[a-z0-9_]{1,24}\Z")
RE_LOCAL = re.compile(r"[a-z0-9_]{1,24}\Z")          # tile, sound set and alias names
RE_BLOCK_ID = re.compile(r"[a-z0-9_/]{1,48}\Z")      # the path half of a K.1 D15 name


class Stock(object):
    """What the stock pack compiles to: rows 0..127 of every per-type table and the stock lists."""

    def __init__(self):
        self.info = [0] * STOCK_IDS
        self.color = [(0, 0, 0)] * STOCK_IDS
        self.faces = [(0,) * 6] * STOCK_IDS
        self.hud = [0] * STOCK_IDS
        self.tnt = [0] * STOCK_IDS
        self.sound_set = [0] * STOCK_IDS
        self.paint_art = [0] * STOCK_IDS
        self.names = [None] * STOCK_IDS
        self.aliases = []            # (name, id) in document order
        self.pages = [[], []]
        self.twin = [0] * ATLAS_TILES
        self.particle_art = [0] * ATLAS_TILES
        self.sets = []               # (full name, build, break, land, step, tnt)
        self.tile_names = {}         # K.2-4: eden tile name -> (atlas, index), for mod packs' faces


def compile_pack(data, label="eden"):
    """-> (Stock or None, [error lines]). Every refusal is `<label>@<offset>: <message>`; the engine
    prints the same lines in the same order."""
    errs = []

    def err(off, msg):
        errs.append("%s@%d: %s" % (label, off, msg))

    try:
        root = json_parse(data)
    except JsonError as e:
        err(e.offset, "JSON: " + e.message)
        return None, errs
    if root.t != "object":
        err(root.off, "pack.json must be an object")
        return None, errs
    for k, v in root.v:
        if k not in TOP_KEYS:
            err(v.off, 'unknown key "%s"' % k)
    st = Stock()

    fmt = root.get("format")
    if fmt is None:
        err(root.off, 'missing "format"')
    elif as_int(fmt, 1, 1) is None:
        err(fmt.off, '"format" must be 1 (this build reads format 1)')
    pid = root.get("id")
    if pid is None:
        err(root.off, 'missing "id"')
    elif pid.t != "string" or not RE_PACK_ID.match(pid.v):
        err(pid.off, '"id" must match [a-z0-9_]{1,24}')
    elif pid.v != "eden":
        err(pid.off, 'the stock pack\'s "id" must be "eden"')
    for k in ("name", "version"):
        v = root.get(k)
        if v is not None and v.t != "string":
            err(v.off, '"%s" must be a string' % k)

    # ---- tiles
    tiles = {}   # name -> (atlas, index, value)
    tv = root.get("tiles")
    if tv is None:
        err(root.off, 'missing "tiles"')
    elif tv.t != "object":
        err(tv.off, '"tiles" must be an object')
    else:
        slots = {}
        for name, t in tv.v:
            ctx = 'tile "%s": ' % name
            if not RE_LOCAL.match(name):
                err(t.off, ctx + "the name must match [a-z0-9_]{1,24}")
                continue
            if t.t != "object":
                err(t.off, ctx + "must be an object")
                continue
            bad = False
            for k, v in t.v:
                if k not in TILE_KEYS:
                    err(v.off, ctx + 'unknown key "%s"' % k)
                    bad = True
            a, ix = t.get("atlas"), t.get("index")
            if a is None or as_int(a, 1, 2) is None:
                err(a.off if a else t.off, ctx + '"atlas" must be 1 or 2')
                continue
            if ix is None or as_int(ix, 0, ATLAS_TILES - 1) is None:
                err(ix.off if ix else t.off, ctx + '"index" must be 0..127')
                continue
            key = (as_int(a, 1, 2), as_int(ix, 0, 127))
            if key in slots:
                err(t.off, ctx + 'atlas %d index %d is already tile "%s"' % (key[0], key[1], slots[key]))
                continue
            slots[key] = name
            if not bad:
                tiles[name] = (key[0], key[1], t)
                st.tile_names[name] = key
        for name, t in tv.v:
            if name not in tiles:
                continue
            atlas, index, t = tiles[name]
            ctx = 'tile "%s": ' % name
            u, pa = t.get("unpainted"), t.get("particle_art")
            if pa is not None and pa.t != "bool":
                err(pa.off, ctx + '"particle_art" must be true or false')
            elif pa is not None and pa.v:
                if atlas != 1:
                    err(pa.off, ctx + "particle_art is for atlas 1 tiles only")
                else:
                    st.particle_art[index] = 1
            if u is not None:
                if u.t != "string":
                    err(u.off, ctx + '"unpainted" must be a tile name')
                elif u.v not in tiles:
                    err(u.off, ctx + 'unknown unpainted tile "%s"' % u.v)
                elif atlas != 1 or tiles[u.v][0] != 1 or tiles[u.v][1] == 0:
                    err(u.off, ctx + "an unpainted twin pairs two atlas 1 tiles, and index 0 cannot be one")
                else:
                    st.twin[index] = tiles[u.v][1]

    # ---- sound sets
    sets = {}    # local name -> index
    sv = root.get("sound_sets")
    if sv is None:
        err(root.off, 'missing "sound_sets"')
    elif sv.t != "object":
        err(sv.off, '"sound_sets" must be an object')
    elif not sv.v:
        err(sv.off, '"sound_sets" is empty (the first set is every block\'s default)')
    elif len(sv.v) > SOUND_SETS_MAX:
        err(sv.off, '"sound_sets" has %d sets (at most %d)' % (len(sv.v), SOUND_SETS_MAX))
    else:
        for name, s in sv.v:
            ctx = 'sound set "%s": ' % name
            if not RE_LOCAL.match(name):
                err(s.off, ctx + "the name must match [a-z0-9_]{1,24}")
                continue
            if s.t != "object":
                err(s.off, ctx + "must be an object")
                continue
            ok = True
            for k, v in s.v:
                if k not in SET_KEYS:
                    err(v.off, ctx + 'unknown key "%s"' % k)
                    ok = False
            row = []
            for k in ("build", "break", "land", "step"):
                v = s.get(k)
                if v is None and k in ("build", "break"):
                    err(s.off, ctx + 'missing "%s"' % k)
                    ok = False
                elif v is None or v.t == "null":
                    if k in ("build", "break"):
                        err(v.off, ctx + '"%s" must name a sound' % k)
                        ok = False
                    row.append(-1)
                elif v.t != "string" or v.v not in SOUNDS:
                    err(v.off, ctx + 'unknown sound "%s"' % (v.v if v.t == "string" else "?"))
                    ok = False
                else:
                    row.append(SOUNDS[v.v])
            g = s.get("tnt")
            if g is None:
                err(s.off, ctx + 'missing "tnt"')
                ok = False
            elif g.t != "string" or g.v not in TNT_GROUPS:
                err(g.off, ctx + '"tnt" must be one of other, dirt, stone, glass')
                ok = False
            if ok:
                sets[name] = len(st.sets)
                st.sets.append(("eden:" + name, row[0], row[1], row[2], row[3], TNT_GROUPS.index(g.v)))

    # ---- blocks
    def tile_ref(s):
        if s.startswith("eden:"):
            s = s[5:]
        return tiles.get(s)

    by_name = {}       # "eden:<id>" -> stock id
    deferred = []      # (stock id, ctx, key, value) for bt / tnt_map
    bv = root.get("blocks")
    if bv is None:
        err(root.off, 'missing "blocks"')
    elif bv.t != "array":
        err(bv.off, '"blocks" must be an array')
    elif len(bv.v) > BLOCKS_MAX:
        err(bv.off, '"blocks" has %d blocks (at most %d)' % (len(bv.v), BLOCKS_MAX))
    else:
        ids = set()
        for n, b in enumerate(bv.v):
            if b.t != "object":
                err(b.off, "block #%d: must be an object" % n)
                continue
            idv = b.get("id")
            if idv is not None and idv.t == "string" and RE_BLOCK_ID.match(idv.v):
                ctx = 'block "%s": ' % idv.v
            else:
                ctx = "block #%d: " % n
                err(idv.off if idv else b.off, ctx + '"id" must match [a-z0-9_/]{1,48}')
                continue
            for k, v in b.v:
                if k not in BLOCK_KEYS:
                    err(v.off, ctx + 'unknown key "%s"' % k)
            if idv.v in ids:
                err(idv.off, ctx + "duplicate block id")
                continue
            ids.add(idv.v)
            sid_v = b.get("stock_id")
            sid = as_int(sid_v, 0, STOCK_IDS - 1) if sid_v is not None else None
            if sid is None:
                err(sid_v.off if sid_v else b.off, ctx + '"stock_id" must be 0..127')
                continue
            if st.names[sid] is not None:
                err(sid_v.off, ctx + 'stock_id %d is already "%s"' % (sid, st.names[sid]))
                continue
            kind = b.get("kind")
            if kind is not None and (kind.t != "string" or kind.v not in KINDS):
                err(kind.off, ctx + 'unknown kind "%s"' % (kind.v if kind.t == "string" else "?"))
            info = IS_NOTSOLID
            art = 0
            seen = set()
            tr = b.get("traits")
            if tr is not None:
                if tr.t != "array":
                    err(tr.off, ctx + '"traits" must be an array of names')
                else:
                    for t in tr.v:
                        if t.t != "string" or t.v not in TRAITS:
                            err(t.off, ctx + 'unknown trait "%s"' % (t.v if t.t == "string" else "?"))
                        elif t.v in seen:
                            err(t.off, ctx + 'trait "%s" listed twice' % t.v)
                        else:
                            seen.add(t.v)
                            if t.v == "solid":
                                info &= ~IS_NOTSOLID
                            elif t.v == "paint_art":
                                art = ART_ALL
                            else:
                                info |= TRAITS[t.v]
            fl = b.get("flags")
            if fl is not None:
                if fl.t != "array":
                    err(fl.off, ctx + '"flags" must be an array of names')
                else:
                    for f in fl.v:
                        if f.t != "string" or f.v not in FLAGS:
                            err(f.off, ctx + 'unknown flag "%s"' % (f.v if f.t == "string" else "?"))
                        elif info & FLAGS[f.v]:
                            err(f.off, ctx + 'flag "%s" listed twice' % f.v)
                        else:
                            info |= FLAGS[f.v]
            sites = b.get("paint_art_sites")
            if sites is not None:
                if "paint_art" in seen:
                    err(sites.off, ctx + '"paint_art_sites" and the "paint_art" trait together')
                elif sites.t != "array":
                    err(sites.off, ctx + '"paint_art_sites" must be an array of site names')
                else:
                    for s in sites.v:
                        if s.t != "string" or s.v not in SITES:
                            err(s.off, ctx + 'unknown paint art site "%s"' % (s.v if s.t == "string" else "?"))
                        elif art & (1 << SITES.index(s.v)):
                            err(s.off, ctx + 'paint art site "%s" listed twice' % s.v)
                        else:
                            art |= 1 << SITES.index(s.v)
            atlas = 2 if info & IS_ATLAS2 else 1
            faces = [0] * 6
            fv = b.get("faces")
            if fv is not None:
                keys = [k for k, _ in fv.v] if fv.t == "object" else None
                if keys is not None and sorted(keys) == ["all"]:
                    want = [("all", list(range(6)))]
                elif keys is not None and sorted(keys) == ["bottom", "sides", "top"]:
                    want = [("sides", [0, 1, 2, 3]), ("bottom", [4]), ("top", [5])]
                elif keys is not None and sorted(keys) == sorted(FACE_KEYS):
                    want = [(k, [i]) for i, k in enumerate(FACE_KEYS)]
                else:
                    err(fv.off, ctx + '"faces" must be {"all"}, {"sides","top","bottom"} or the six '
                                      '"front","back","left","right","bottom","top"')
                    want = []
                for k, idx in want:
                    v = fv.get(k)
                    if v.t != "string" or tile_ref(v.v) is None:
                        err(v.off, ctx + 'unknown tile "%s"' % (v.v if v.t == "string" else "?"))
                        continue
                    ta, ti, _ = tile_ref(v.v)
                    if ta != atlas:
                        err(v.off, ctx + 'tile "%s" is in atlas %d but the block draws from atlas %d'
                            % (v.v, ta, atlas))
                        continue
                    for i in idx:
                        faces[i] = ti
            cv = b.get("color")
            rgb = None
            if cv is None:
                err(b.off, ctx + 'missing "color"')
            elif cv.t != "array" or len(cv.v) != 3 or any(as_int(c, 0, 255) is None for c in cv.v):
                err(cv.off, ctx + '"color" must be [r, g, b] with each 0..255')
            else:
                rgb = tuple(as_int(c, 0, 255) for c in cv.v)
            ss = 0
            sn = b.get("sounds")
            if sn is not None:
                if sn.t != "string" or not sn.v.startswith("eden:") or sn.v[5:] not in sets:
                    err(sn.off, ctx + 'unknown sound set "%s"' % (sn.v if sn.t == "string" else "?"))
                else:
                    ss = sets[sn.v[5:]]
            st.names[sid] = "eden:" + idv.v
            by_name["eden:" + idv.v] = sid
            st.info[sid] = info
            st.faces[sid] = tuple(faces)
            st.color[sid] = rgb or (0, 0, 0)
            st.sound_set[sid] = ss
            st.paint_art[sid] = art
            for key in ("bt", "tnt_map"):
                v = b.get(key)
                if v is not None:
                    deferred.append((sid, ctx, key, v))
        for sid, ctx, key, v in deferred:
            if key == "bt" and v.t == "null":
                st.hud[sid] = -1
            elif v.t != "string" or v.v not in by_name:
                err(v.off, ctx + '"%s": unknown block "%s"' % (key, v.v if v.t == "string" else "?"))
            elif key == "bt":
                st.hud[sid] = by_name[v.v]
            else:
                st.tnt[sid] = by_name[v.v]
        missing = [str(i) for i in range(STOCK_IDS) if st.names[i] is None]
        if missing:
            err(bv.off, "stock ids without a block: " + ", ".join(missing))

    # ---- aliases
    av = root.get("aliases")
    if av is not None:
        if av.t != "object":
            err(av.off, '"aliases" must be an object')
        else:
            for name, v in av.v:
                if not RE_LOCAL.match(name):
                    err(v.off, 'alias "%s": the name must match [a-z0-9_]{1,24}' % name)
                elif v.t != "string" or v.v not in by_name:
                    err(v.off, 'alias "%s": unknown block "%s"' % (name, v.v if v.t == "string" else "?"))
                else:
                    st.aliases.append((name, by_name[v.v]))

    # ---- picker pages
    pv = root.get("picker_pages")
    if pv is None:
        err(root.off, 'missing "picker_pages"')
    elif pv.t != "array" or len(pv.v) != 2:
        err(pv.off, '"picker_pages" must be an array of the 2 stock pages')
    else:
        for page, cells in enumerate(pv.v):
            if cells.t != "array" or len(cells.v) != PICKER_PAGE_CELLS[page]:
                err(cells.off, "picker page %d must have %d cells (the stock grid)" % (page, PICKER_PAGE_CELLS[page]))
                continue
            for c in cells.v:
                if c.t == "string" and c.v in TOOLS:
                    st.pages[page].append(TOOLS[c.v])
                elif c.t == "string" and c.v in by_name:
                    st.pages[page].append(by_name[c.v])
                else:
                    err(c.off, 'picker page %d: unknown block "%s"' % (page, c.v if c.t == "string" else "?"))
    return (None if errs else st), errs


# ---------------------------------------------------------------------------------------------
# K.2-4: mod packs (docs/content-packs.md "Mod packs"; the engine's copy is ContentPack::compileMod and
# ContentPack::loadMods). Same rules, same order, same lines.

MOD_TOP_KEYS = ["format", "id", "name", "version", "requires", "conflicts", "tiles", "blocks", "scripts"]
MOD_TILE_KEYS = ["png", "unpainted"]
MOD_BLOCK_KEYS = ["id", "kind", "traits", "faces", "color", "sounds", "fallback", "fallback_paint", "picker"]
STOCK_ONLY_BLOCK_KEYS = ["stock_id", "flags", "paint_art_sites", "bt", "tnt_map"]
RESERVED_IDS = ["eden", "emod"]
PNG_MAX_BYTES = 1 << 20
TILE_PX = 32
ATLAS1_FREE_FIRST, ATLAS2_FREE_FIRST = 66, 32
ATLAS2_FRAMES = 8   # an atlas2 tile is an 8-frame strip (Terrain::render2 animates the transparent pass)
TEX_MISSING = 65
DEPS_MAX = 32   # K.3: entries in "requires" / "conflicts"
SCRIPTS_MAX, SCRIPT_MAX_BYTES, SCRIPTS_MAX_BYTES = 16, 256 * 1024, 1024 * 1024   # K.4: "scripts"
RE_REQUIRE = re.compile(r"^([a-z0-9_]{1,24})(?:>=([0-9]+(?:\.[0-9]+)*))?$")


def parse_require(s):
    """K.3: `id` or `id>=1.2` -> (id, min version or ""), or None (ContentPack parseRequire)."""
    m = RE_REQUIRE.match(s)
    if not m or (m.group(2) is not None and len(m.group(2)) > 24):
        return None
    return (m.group(1), m.group(2) or "")


def compare_versions(a, b):
    """ContentPack::compareVersions: dotted numbers, each the leading digits of its part (none = 0)."""
    def parts(s):
        out = []
        for p in s.split(".") if s else []:
            m = re.match(r"[0-9]*", p).group(0)
            out.append(min(int(m), 10 ** 10) if m else 0)
        return out
    x, y = parts(a), parts(b)
    n = max(len(x), len(y))
    x += [0] * (n - len(x)); y += [0] * (n - len(y))
    return (x > y) - (x < y)


def rel_path_ok(p):
    if not p or len(p) > 200 or p[0] == "/":
        return False
    if any(c in ("", ".", "..") for c in p.split("/")):
        return False
    return not any(c in "\\:" or ord(c) < 0x20 for c in p)


def png_size(b):
    """The PNG signature and IHDR: (w, h), or None when not a PNG."""
    if len(b) < 33 or b[:8] != b"\x89PNG\r\n\x1a\n" or b[12:16] != b"IHDR" or int.from_bytes(b[8:12], "big") != 13:
        return None
    w, h = int.from_bytes(b[16:20], "big"), int.from_bytes(b[20:24], "big")
    return min(w, 100000), min(h, 100000)


def png_decodes(b):
    """Whether the image data inflates (the engine asks stb_image; this checks the zlib stream)."""
    import zlib
    at, idat = 8, b""
    while at + 8 <= len(b):
        n = int.from_bytes(b[at:at + 4], "big")
        t = b[at + 4:at + 8]
        if at + 12 + n > len(b):
            return False
        if t == b"IDAT":
            idat += b[at + 8:at + 8 + n]
        if t == b"IEND":
            break
        at += 12 + n
    try:
        return len(zlib.decompress(idat)) > 0
    except zlib.error:
        return False


class ModPack(object):
    def __init__(self, folder, d):
        self.folder, self.dir = folder, d
        self.id = self.name = self.version = ""
        self.needs, self.conflicts = [], []   # K.3: (id, min version) / ids
        self.state = "refused"                # loaded / refused / needs / conflict
        self.blocks_in_file = 0
        self.tiles = []      # (name, twin index or -1)
        self.scripts = []    # K.4: (path, bytes), list order
        self.blocks = []     # dicts, the valid ones in file order
        self.lines = []
        self.refused = True


def compile_mod(data, d, label, stock):
    """-> ModPack. `stock` is the compiled eden pack (tiles, sound sets, names)."""
    pk = ModPack(label, d)
    errs = pk.lines

    def err(off, msg):
        errs.append("%s@%d: %s" % (label, off, msg))

    try:
        root = json_parse(data)
    except JsonError as e:
        err(e.offset, "JSON: " + e.message)
        return pk
    if root.t != "object":
        err(root.off, "pack.json must be an object")
        return pk
    top = True
    for k, v in root.v:
        if k not in MOD_TOP_KEYS:
            err(v.off, 'warning: unknown key "%s" (ignored)' % k)
    fmt = root.get("format")
    if fmt is None:
        err(root.off, 'missing "format"'); top = False
    elif as_int(fmt, 1, 1) is None:
        err(fmt.off, '"format" must be 1 (this build reads format 1)'); top = False
    pid = root.get("id")
    if pid is None:
        err(root.off, 'missing "id"'); top = False
    elif pid.t != "string" or not RE_PACK_ID.match(pid.v):
        err(pid.off, '"id" must match [a-z0-9_]{1,24}'); top = False
    elif pid.v in RESERVED_IDS:
        err(pid.off, 'the id "%s" is reserved' % pid.v); top = False
    else:
        pk.id = pid.v
    for k in ("name", "version"):
        v = root.get(k)
        if v is not None and v.t != "string":
            err(v.off, '"%s" must be a string' % k); top = False
        elif v is not None:
            setattr(pk, k, v.v)
    for k in ("requires", "conflicts"):
        v = root.get(k)
        if v is None:
            continue
        if v.t != "array":
            err(v.off, '"%s" must be an array of pack ids' % k); top = False; continue
        if len(v.v) > DEPS_MAX:
            err(v.off, '"%s" has %d entries (at most %d)' % (k, len(v.v), DEPS_MAX)); top = False; continue
        for e in v.v:
            req = k == "requires"
            r = (parse_require(e.v) if req else ((e.v, "") if RE_PACK_ID.match(e.v) else None)) if e.t == "string" else None
            if r is None:
                err(e.off, ('"requires" entry "%s": must be <id> or <id>>=<version> (digits and dots)' if req
                            else '"conflicts" entry "%s": must be a pack id') % (e.v if e.t == "string" else "?")); top = False; continue
            if pk.id and r[0] == pk.id:
                err(e.off, '"%s" names the pack itself' % k); top = False; continue
            (pk.needs if req else pk.conflicts).append(r)
    tv = root.get("tiles")
    if tv is not None and tv.t != "object":
        err(tv.off, '"tiles" must be an object'); top = False
    # K.4 (mod-scripting plan S2): Lua sources. A bad entry refuses the pack; with scripts, "blocks" is optional.
    sv = root.get("scripts")
    if sv is not None:
        total = 0
        if sv.t != "array":
            err(sv.off, '"scripts" must be an array of paths'); top = False
        elif len(sv.v) > SCRIPTS_MAX:
            err(sv.off, '"scripts" has %d entries (at most %d)' % (len(sv.v), SCRIPTS_MAX)); top = False
        else:
            for e in sv.v:
                p = e.v if e.t == "string" else ""
                if e.t != "string" or not rel_path_ok(p) or len(p) < 5 or not p.endswith(".lua"):
                    err(e.off, 'script "%s": must be a relative path inside the pack ending in .lua' % (p if e.t == "string" else "?")); top = False; continue
                if any(q == p for q, _ in pk.scripts):
                    err(e.off, 'script "%s": listed twice' % p); top = False; continue
                try:
                    with open(os.path.join(d, p), "rb") as f:
                        b = f.read(SCRIPT_MAX_BYTES + 1)
                except (IOError, OSError):
                    err(e.off, 'script "%s": cannot read it' % p); top = False; continue
                if len(b) > SCRIPT_MAX_BYTES:
                    err(e.off, 'script "%s": larger than 256 KiB' % p); top = False; continue
                if b[:1] == b"\x1b":
                    err(e.off, 'script "%s": precompiled Lua (only source scripts load)' % p); top = False; continue
                total += len(b)
                pk.scripts.append((p, b))
        if total > SCRIPTS_MAX_BYTES:
            err(sv.off, '"scripts" add up to more than 1 MiB'); top = False
    bv = root.get("blocks")
    if bv is None:
        if sv is None:
            err(root.off, 'missing "blocks"'); top = False
    elif bv.t != "array":
        err(bv.off, '"blocks" must be an array'); top = False
    elif len(bv.v) > BLOCKS_MAX:
        err(bv.off, '"blocks" has %d blocks (at most %d)' % (len(bv.v), BLOCKS_MAX)); top = False
    if not top:
        return pk

    # ---- tiles
    tiles = {}   # name -> index into pk.tiles
    if tv is not None:
        for name, t in tv.v:
            ctx = 'tile "%s": ' % name
            if not RE_LOCAL.match(name):
                err(t.off, ctx + "the name must match [a-z0-9_]{1,24}")
                continue
            if t.t != "object":
                err(t.off, ctx + "must be an object")
                continue
            for k, v in t.v:
                if k not in MOD_TILE_KEYS:
                    err(v.off, 'warning: ' + ctx + 'unknown key "%s" (ignored)' % k)
            png = t.get("png")
            if png is None:
                err(t.off, ctx + 'missing "png"')
                continue
            if png.t != "string" or not rel_path_ok(png.v):
                err(png.off, ctx + '"png" must be a relative path inside the pack')
                continue
            try:
                with open(os.path.join(d, png.v), "rb") as f:
                    b = f.read(PNG_MAX_BYTES + 1)
            except (OSError, IOError):
                err(png.off, ctx + 'cannot read "%s"' % png.v)
                continue
            if len(b) > PNG_MAX_BYTES:
                err(png.off, ctx + '"%s" is larger than 1 MiB' % png.v)
                continue
            wh = png_size(b)
            if wh is None:
                err(png.off, ctx + '"%s" is not a PNG' % png.v)
                continue
            if wh != (TILE_PX, TILE_PX):
                err(png.off, ctx + '"%s" is %dx%d (a tile is 32x32)' % (png.v, wh[0], wh[1]))
                continue
            if not png_decodes(b):
                err(png.off, ctx + '"%s" does not decode' % png.v)
                continue
            tiles[name] = len(pk.tiles)
            pk.tiles.append([name, -1])
        bad_twin = set()
        for name, t in tv.v:
            if name not in tiles:
                continue
            u = t.get("unpainted")
            if u is None:
                continue
            if u.t != "string" or u.v not in tiles:
                err(u.off, 'tile "%s": unknown unpainted tile "%s"' % (name, u.v if u.t == "string" else "?"))
                bad_twin.add(name)
            else:
                pk.tiles[tiles[name]][1] = tiles[u.v]
        for name in bad_twin:
            del tiles[name]

    # ---- blocks
    own = pk.id + ":"
    set_index = {s[0]: i for i, s in enumerate(stock.sets)}
    ids = set()
    pk.blocks_in_file = len(bv.v) if bv is not None else 0
    for n, b in enumerate(bv.v if bv is not None else []):
        if b.t != "object":
            err(b.off, "block #%d: must be an object" % n)
            continue
        idv = b.get("id")
        if not (idv is not None and idv.t == "string" and RE_BLOCK_ID.match(idv.v)):
            err(idv.off if idv else b.off, 'block #%d: "id" must match [a-z0-9_/]{1,48}' % n)
            continue
        ctx = 'block "%s": ' % idv.v
        before = len(errs)
        warnings = 0
        for k, v in b.v:
            if k in STOCK_ONLY_BLOCK_KEYS:
                err(v.off, ctx + '"%s" is for the stock pack only' % k)
            elif k not in MOD_BLOCK_KEYS:
                err(v.off, "warning: " + ctx + 'unknown key "%s" (ignored)' % k)
                warnings += 1
        if idv.v in ids:
            err(idv.off, ctx + "duplicate block id")
            continue
        ids.add(idv.v)
        kind = b.get("kind")
        if kind is not None and (kind.t != "string" or kind.v not in KINDS):
            err(kind.off, ctx + 'unknown kind "%s"' % (kind.v if kind.t == "string" else "?"))
        elif kind is not None and kind.v != "cube":
            err(kind.off, ctx + 'kind "%s" is for the stock pack only (a mod block is "cube")' % kind.v)
        info = IS_NOTSOLID
        art = 0
        seen = set()
        tr = b.get("traits")
        if tr is not None:
            if tr.t != "array":
                err(tr.off, ctx + '"traits" must be an array of names')
            else:
                for t in tr.v:
                    if t.t != "string" or t.v not in TRAITS:
                        err(t.off, ctx + 'unknown trait "%s"' % (t.v if t.t == "string" else "?"))
                    elif t.v in seen:
                        err(t.off, ctx + 'trait "%s" listed twice' % t.v)
                    else:
                        seen.add(t.v)
                        if t.v == "solid":
                            info &= ~IS_NOTSOLID
                        elif t.v == "paint_art":
                            art = ART_ALL
                        else:
                            info |= TRAITS[t.v]
        atlas = 2 if info & IS_ATLAS2 else 1
        faces = [(0, -1)] * 6     # (eden index, local tile)
        fv = b.get("faces")
        if fv is None:
            err(b.off, ctx + 'missing "faces"')
        else:
            keys = [k for k, _ in fv.v] if fv.t == "object" else None
            if keys is not None and sorted(keys) == ["all"]:
                want = [("all", list(range(6)))]
            elif keys is not None and sorted(keys) == ["bottom", "sides", "top"]:
                want = [("sides", [0, 1, 2, 3]), ("bottom", [4]), ("top", [5])]
            elif keys is not None and sorted(keys) == sorted(FACE_KEYS):
                want = [(k, [i]) for i, k in enumerate(FACE_KEYS)]
            else:
                err(fv.off, ctx + '"faces" must be {"all"}, {"sides","top","bottom"} or the six '
                                  '"front","back","left","right","bottom","top"')
                want = []
            for k, idx in want:
                v = fv.get(k)
                face = None
                if v.t == "string" and v.v.startswith("eden:"):
                    et = stock.tile_names.get(v.v[5:])
                    if et is not None:
                        if et[0] != atlas:
                            err(v.off, ctx + 'tile "%s" is in atlas %d but the block draws from atlas %d' % (v.v, et[0], atlas))
                            continue
                        face = (et[1], -1)
                elif v.t == "string":
                    local = v.v[len(own):] if v.v.startswith(own) else v.v
                    if local in tiles:
                        face = (0, tiles[local])
                if face is None:
                    err(v.off, ctx + 'unknown tile "%s"' % (v.v if v.t == "string" else "?"))
                    continue
                for i in idx:
                    faces[i] = face
        cv = b.get("color")
        rgb = (0, 0, 0)
        if cv is None:
            err(b.off, ctx + 'missing "color"')
        elif cv.t != "array" or len(cv.v) != 3 or any(as_int(c, 0, 255) is None for c in cv.v):
            err(cv.off, ctx + '"color" must be [r, g, b] with each 0..255')
        else:
            rgb = tuple(as_int(c, 0, 255) for c in cv.v)
        ss = 0
        sn = b.get("sounds")
        if sn is not None:
            if sn.t != "string" or sn.v not in set_index:
                err(sn.off, ctx + 'unknown sound set "%s"' % (sn.v if sn.t == "string" else "?"))
            else:
                ss = set_index[sn.v]
        fb = b.get("fallback")
        fb_type = 0
        if fb is None:
            err(b.off, ctx + 'missing "fallback"')
        else:
            t = stock.names.index(fb.v) if fb.t == "string" and fb.v.startswith("eden:") and fb.v in stock.names else -1
            if not (1 <= t <= 127 and not 66 <= t <= 70 and not 75 <= t <= 80):
                err(fb.off, ctx + '"fallback" must name a placeable stock block ("eden:..."; not air, a door or portal half, or custom)')
            else:
                fb_type = t
        fb_paint = 0
        fp = b.get("fallback_paint")
        if fp is not None:
            if as_int(fp, 0, 54) is None:
                err(fp.off, ctx + '"fallback_paint" must be 0..54')
            else:
                fb_paint = as_int(fp, 0, 54)
        picker = True
        pv = b.get("picker")
        if pv is not None:
            if pv.t != "bool":
                err(pv.off, ctx + '"picker" must be true or false')
            else:
                picker = pv.v
        if len(errs) - before != warnings:
            continue
        pk.blocks.append({"id": idv.v, "info": info, "faces": faces, "color": rgb, "sound_set": ss, "paint_art": art,
                          "fallback": fb_type, "fallback_paint": fb_paint, "picker": picker})
    pk.refused = False
    return pk


def pack_dirs(roots):
    """ContentPack::loadMods' scan of the --packs= roots: a pack folder, or a folder of packs (bytewise)."""
    out = []
    for r in roots:
        r = r.rstrip("/") or "/"
        if os.path.isfile(os.path.join(r, "pack.json")):
            out.append(r)
        elif os.path.isdir(r):
            for n in sorted(os.listdir(r), key=lambda x: x.encode()):
                if n.startswith("."):
                    continue
                if os.path.isdir(os.path.join(r, n)):
                    out.append(os.path.join(r, n))
    return out


def resolve_requires(packs, lines):
    """ContentPack resolveRequires: a fixpoint (a skipped pack can break the packs that need it)."""
    changed = True
    while changed:
        changed = False
        for pk in packs:
            if pk.state != "loaded":
                continue
            for rid, minv in pk.needs:
                q = next((o for o in packs if o.state == "loaded" and o.id == rid), None)
                why = ""
                if q is None:
                    why = 'needs "%s" (%s)' % (rid, "not loaded" if any(o.id == rid for o in packs) else "not installed")
                elif minv and compare_versions(q.version, minv) < 0:
                    why = 'needs "%s" %s or newer (found "%s")' % (rid, minv, q.version)
                if not why:
                    continue
                pk.state, pk.refused = "needs", True
                lines.append("%s: %s (skipped)" % (pk.folder, why))
                changed = True
                break


def scan_mods(roots, stock, lines):
    """ContentPack::scanPacks for --packs= roots only (no enable list): compile, de-duplicate, resolve."""
    packs = []
    for d in pack_dirs(roots):
        folder = os.path.basename(d.rstrip("/"))
        p = os.path.join(d, "pack.json")
        try:
            with open(p, "rb") as f:
                data = f.read(MAX_BYTES + 1)
        except (OSError, IOError):
            lines.append("%s: no pack.json (skipped)" % folder)
            continue
        pk = compile_mod(data, d, folder, stock)
        pk.state = "refused" if pk.refused else "loaded"
        packs.append(pk)
    id_folder = {}
    for pk in packs:
        if pk.state != "loaded":
            continue
        if pk.id in id_folder:
            lines.append('%s: the pack id "%s" is already loaded from %s (skipped)' % (pk.folder, pk.id, id_folder[pk.id]))
            pk.refused, pk.state = True, "refused"
        else:
            id_folder[pk.id] = pk.folder
    resolve_requires(packs, lines)
    for i, pk in enumerate(packs):
        if pk.state != "loaded":
            continue
        for q in packs[:i]:
            if q.state == "loaded" and (any(c[0] == q.id for c in pk.conflicts) or any(c[0] == pk.id for c in q.conflicts)):
                pk.state, pk.refused = "conflict", True
                lines.append('%s: conflicts with "%s" (skipped)' % (pk.folder, q.id))
                break
    resolve_requires(packs, lines)
    return packs


def load_mods(roots, stock):
    """-> the `## mods` text the engine prints for `--content-dump --packs=<roots>`."""
    packs, lines, blocks, tiles = [], [], [], []
    twin = {}
    slot = {}
    nxt = {1: ATLAS1_FREE_FIRST, 2: ATLAS2_FREE_FIRST}
    packs = scan_mods(roots, stock, lines)
    for pi, pk in enumerate(packs):
        if pk.state == "loaded":
            for mb in pk.blocks:
                atlas = 2 if mb["info"] & IS_ATLAS2 else 1
                need = []
                for _, local in mb["faces"]:
                    if local < 0:
                        continue
                    for k in [(local, atlas)] + ([(pk.tiles[local][1], 1)] if atlas == 1 and pk.tiles[local][1] >= 0 else []):
                        if (pi, k[0], k[1]) not in slot and k not in need:
                            need.append(k)
                count = {1: sum(1 for k in need if k[1] == 1), 2: ATLAS2_FRAMES * sum(1 for k in need if k[1] == 2)}
                info = mb["info"]
                missing = False
                if nxt[1] + count[1] > ATLAS_TILES or nxt[2] + count[2] > ATLAS_TILES:
                    missing = True
                    info &= ~IS_ATLAS2
                    faces = [TEX_MISSING] * 6
                    lines.append('%s: block "%s": no free atlas tile (%d opaque and %d transparent are taken): '
                                 'drawn as the missing texture' % (folder, mb["id"], ATLAS_TILES - ATLAS1_FREE_FIRST,
                                                                   (ATLAS_TILES - ATLAS2_FREE_FIRST) // ATLAS2_FRAMES))
                else:
                    for local, a in need:
                        slot[(pi, local, a)] = nxt[a]
                        tiles.append((a, nxt[a], pk.id + ":" + pk.tiles[local][0]))
                        nxt[a] += ATLAS2_FRAMES if a == 2 else 1
                    faces = [e if local < 0 else slot[(pi, local, atlas)] for e, local in mb["faces"]]
                    if atlas == 1:
                        for _, local in mb["faces"]:
                            if local >= 0 and pk.tiles[local][1] >= 0:
                                twin[slot[(pi, local, 1)]] = slot[(pi, pk.tiles[local][1], 1)]
                blocks.append((pk.id + ":" + mb["id"], info, mb, faces, pi))
    o = ["## mods", "# pack folder id blocks-loaded/blocks-in-file refused"]
    for i, pk in enumerate(packs):
        o.append("pack %s %s %d/%d %d" % (pk.folder, pk.id or "-", sum(1 for b in blocks if b[4] == i),
                                          pk.blocks_in_file, 1 if pk.refused else 0))
        o += ["line " + l for l in pk.lines]
    o += ["loader " + l for l in lines]
    o.append("# block name info color faces(6) sound_set paint_art fallback paint picker")
    for name, info, mb, faces, _ in blocks:
        o.append("block %s %08x %s %s %s %02x %s %d %d" % (name, info, ",".join(map(str, mb["color"])), ",".join(map(str, faces)),
                                                           stock.sets[mb["sound_set"]][0], mb["paint_art"],
                                                           stock.names[mb["fallback"]], mb["fallback_paint"], 1 if mb["picker"] else 0))
    o.append("# tile atlas index name")
    o += ["tile %d %d %s" % t for t in tiles]
    o.append("# twin tile unpainted-twin (atlas 1, mod tiles)")
    o += ["twin %d %d" % (k, twin[k]) for k in sorted(twin) if twin[k]]
    return "\n".join(o) + "\n"


def load_stock_for_mods(stock_dir):
    data, e = read_pack(stock_dir)
    if e:
        raise SystemExit(e[0])
    st, errs = compile_pack(data)
    if not st:
        raise SystemExit("the stock pack %s does not compile: %s" % (stock_dir, errs[0]))
    return st


# ---------------------------------------------------------------------------------------------
# The `--content-dump` text (port/src/harness/mode_content.cpp), from compiled rows. Rows 128..255
# are the "missing" block's (Globals.cpp eden_set_missing_block_row), the pack does not reach them.

def dump_text(st):
    info = list(st.info) + [IS_HARD | IS_MISSING] * (BLOCK_IDS - STOCK_IDS)
    color = list(st.color) + [(255, 0, 255)] * (BLOCK_IDS - STOCK_IDS)
    faces = list(st.faces) + [(65,) * 6] * (BLOCK_IDS - STOCK_IDS)
    hud = list(st.hud) + [-1] * (BLOCK_IDS - STOCK_IDS)
    tnt = list(st.tnt) + [0] * (BLOCK_IDS - STOCK_IDS)
    sset = list(st.sound_set) + [0] * (BLOCK_IDS - STOCK_IDS)
    art = list(st.paint_art) + [0] * (BLOCK_IDS - STOCK_IDS)
    shadow = [0] + [0 if (info[t] & IS_NOSHADOW or info[t] & (IS_LIQUID | IS_OBJECT)) else 1
                    for t in range(1, BLOCK_IDS)]
    o = ["# eden content dump v2 (K.2-2)", "## tables", "# t info color faces(6) hud tnt shadow name"]
    for t in range(BLOCK_IDS):
        o.append("%d %08x %s %s %d %d %d %s" % (t, info[t], ",".join(map(str, color[t])), ",".join(map(str, faces[t])),
                                               hud[t], tnt[t], shadow[t], st.names[t] if t < STOCK_IDS else "-"))
    o.append("## aliases")
    o += ["%s %d" % a for a in st.aliases]
    o.append("## picker")
    for p in range(2):
        o.append("page%d %s" % (p, ",".join(map(str, st.pages[p]))))
    o.append("## oracle")
    o.append("# t build break land step tnt climb bounce slip blast unbreak noshadow "
             "art:chunk,batch,cursor,icon,picker,particle")
    for t in range(BLOCK_IDS):
        s = st.sets[sset[t]]
        bits = [1 if info[t] & b else 0 for b in (IS_CLIMB, IS_BOUNCY, IS_SLIPPERY, IS_BLASTPROOF, IS_UNBREAKABLE, IS_NOSHADOW)]
        arts = [1 if art[t] & (1 << i) else 0 for i in range(6)]
        o.append("%d %d %d %d %d %d %s %s" % (t, s[1], s[2], s[3], s[4], s[5], " ".join(map(str, bits)), ",".join(map(str, arts))))
    o.append("## tiles")
    o.append("# tile twin? twin art:particle(type 0)")
    for bf in range(ATLAS_TILES):
        tw = st.twin[bf]
        o.append("%d %d %d %d" % (bf, 1 if tw else 0, tw if tw else bf, 1 if (st.particle_art[bf] or art[0] & 32) else 0))
    o.append("## sound sets")
    o.append("# index name build break land step tnt")
    for i, s in enumerate(st.sets):
        o.append("%d %s %d %d %d %d %d" % ((i,) + s))
    o.append("## traits")
    o.append("# t sound_set paint_art(sites)")
    for t in range(BLOCK_IDS):
        o.append("%d %s %02x" % (t, st.sets[sset[t]][0], art[t]))
    return "\n".join(o) + "\n"


def extract_dump(path):
    """The text between `[content-dump] BEGIN`/`END` in a log, or the whole file (the fixture)."""
    with open(path, "rb") as f:
        data = f.read().replace(b"\r\n", b"\n")
    m = re.search(rb"^\[content-dump\] BEGIN\n(.*?)^\[content-dump\] END\n", data, re.S | re.M)
    return (m.group(1) if m else data).decode()


# ---------------------------------------------------------------------------------------------
# extract: the one-off that wrote media/packs/eden/pack.json from the K.2-2 dump (kept so the
# derivation stays reviewable; re-running it must reproduce the shipped file byte for byte).

ATLAS1_TILES = ["grass_top", "grass_side_color", "grass_side", "dirt", "sand", "stone", "bedrock", "tree_side",
                "tree_vert", "wood", "tnt_side_color", "tnt_side", "tnt_top_color", "tnt_top", "grass_top2",
                "dark_stone", "leaves", "steel", "block_tnt", "brick_color", "brick", "cobblestone", "crystal",
                "lightbox", "ladder", "cloud", "vine", "shingle", "gradient", "ice", "trampoline", "firework"]
NEW_BLOCKS = ["ore_sand", "space_stone", "carpet", "snakeskin", "obsidian", "cheese", "space_dirt", "space_grass",
              "moss", "dark_matter", "space_sand", "snow", "moonrock", "basalt", "dark_tile", "algae"]
ATLAS1_TILES += [n + "_color" for n in NEW_BLOCKS] + NEW_BLOCKS + ["grass_flower", "missing"]   # 32-47, 48-63, 64, 65
ATLAS2_TILES = {0: "glass", 8: "weave", 16: "water", 24: "lava"}


def stock_kind(t):
    if t == 0: return "air"
    if t in (8, 11, 12): return "grass"
    if t == 9: return "tnt"
    if t in (20, 23) or 59 <= t <= 64: return "liquid"
    if 24 <= t <= 39: return "ramp"
    if 40 <= t <= 55: return "side"
    if t == 65: return "firework"
    if 66 <= t <= 70: return "door"
    if t in (71, 73): return "object"
    if t == 72: return "lightbox"
    if 75 <= t <= 79: return "portal"
    if t == 80: return "custom"
    if t == 81: return "block_tnt"
    if 82 <= t <= 111: return "bt"
    return "cube"


def jstr(s):
    return '"%s"' % s


def jlist(xs):
    return "[" + ", ".join(xs) + "]"


def cmd_extract(dump_path, out_path):
    text = extract_dump(dump_path)
    sec, cur = {}, None
    for line in text.splitlines():
        if line.startswith("## "):
            cur = line[3:]
            sec[cur] = []
        elif not line.startswith("#") and cur:
            sec[cur].append(line)
    rows = [l.split(" ") for l in sec["tables"]][:STOCK_IDS]
    sets = [l.split(" ") for l in sec["sound sets"]]
    traits = [l.split(" ") for l in sec["traits"]][:STOCK_IDS]
    tiles_rows = [list(map(int, l.split(" "))) for l in sec["tiles"]]
    by_id = {int(r[0]): r[7][5:] for r in rows}
    used2 = set()
    for r in rows:
        if int(r[1], 16) & IS_ATLAS2:
            used2.update(int(x) for x in r[3].split(","))
    assert used2 <= set(ATLAS2_TILES), used2

    L = ["// The stock pack: Eden's 128 blocks as data (K.2-3). The engine loads it at boot and it is the only",
         "// source of rows 0..127 of every per-type table; `--content-dump` prints what it compiles to and CI",
         "// pins that text (.github/scripts/check-content-dump.py). Schema: docs/content-packs.md. Written once",
         "// by `port/tools/pack.py extract` from the K.2-2 dump; edit it by hand from now on, and re-pin the dump",
         "// only for a deliberate behaviour change. Never renumber a stock_id (rule 4: saves store the byte).",
         "{",
         '  "format": 1,', '  "id": "eden",', '  "name": "Eden",', '  "version": "2.1.1",',
         '  "tiles": {']
    tl = []
    for i, name in enumerate(ATLAS1_TILES):
        parts = ['"atlas": 1', '"index": %d' % i]
        tr = tiles_rows[i]
        if tr[1]:
            parts.append('"unpainted": "%s"' % ATLAS1_TILES[tr[2]])
        if tr[3]:
            parts.append('"particle_art": true')
        tl.append('    "%s": {%s}' % (name, ", ".join(parts)))
    for i in sorted(ATLAS2_TILES):
        tl.append('    "%s": {"atlas": 2, "index": %d}' % (ATLAS2_TILES[i], i))
    L += [",\n".join(tl), "  },", '  "sound_sets": {']
    sl = []
    for s in sets:
        def snd(v):
            return "null" if int(v) < 0 else jstr(SOUND_NAMES[int(v)])
        sl.append('    "%s": {"build": %s, "break": %s, "land": %s, "step": %s, "tnt": "%s"}'
                  % (s[1][5:], snd(s[2]), snd(s[3]), snd(s[4]), snd(s[5]), TNT_GROUPS[int(s[6])]))
    L += [",\n".join(sl), "  },", '  "blocks": [']
    bl = []
    for t, r in enumerate(rows):
        info = int(r[1], 16)
        color = r[2].split(",")
        faces = r[3].split(",")
        hud, tnt = int(r[4]), int(r[5])
        art = int(traits[t][2], 16)
        names2 = ATLAS2_TILES if info & IS_ATLAS2 else dict(enumerate(ATLAS1_TILES))
        tr = []
        if not info & IS_NOTSOLID: tr.append("solid")
        for n in TRAIT_ORDER[1:-1]:
            if info & TRAITS[n]: tr.append(n)
        if art == ART_ALL: tr.append("paint_art")
        fl = [n for n in FLAG_ORDER if info & FLAGS[n]]
        rest = info & ~(IS_NOTSOLID | sum(TRAITS.values()) | sum(FLAGS.values()))
        assert rest == 0, (t, hex(rest))
        parts = ['"id": "%s"' % r[7][5:], '"stock_id": %d' % t, '"kind": "%s"' % stock_kind(t)]
        if tr: parts.append('"traits": ' + jlist(map(jstr, tr)))
        if fl: parts.append('"flags": ' + jlist(map(jstr, fl)))
        f = [names2[int(x)] for x in faces]
        if t in (0, 80) and faces == ["0"] * 6:
            pass   # air and the custom block have no faces of their own (rows of 0)
        elif len(set(f)) == 1:
            parts.append('"faces": {"all": "%s"}' % f[0])
        elif len(set(f[:4])) == 1:
            parts.append('"faces": {"sides": "%s", "bottom": "%s", "top": "%s"}' % (f[0], f[4], f[5]))
        else:
            parts.append('"faces": {%s}' % ", ".join('"%s": "%s"' % (k, f[i]) for i, k in enumerate(FACE_KEYS)))
        parts.append('"color": [%s]' % ", ".join(color))
        parts.append('"sounds": "%s"' % traits[t][1])
        if art and art != ART_ALL:
            parts.append('"paint_art_sites": ' + jlist(jstr(SITES[i]) for i in range(6) if art & (1 << i)))
        if hud == -1: parts.append('"bt": null')
        elif hud: parts.append('"bt": "eden:%s"' % by_id[hud])
        if tnt: parts.append('"tnt_map": "eden:%s"' % by_id[tnt])
        bl.append("    {%s}" % ", ".join(parts))
    L += [",\n".join(bl), "  ],", '  "aliases": {']
    al = ['    "%s": "eden:%s"' % (a.split(" ")[0], by_id[int(a.split(" ")[1])]) for a in sec["aliases"]]
    L += [",\n".join(al), "  },", '  "picker_pages": [']
    pl = []
    for p in sec["picker"]:
        cells = p.split(" ")[1].split(",")
        inv = {v: k for k, v in TOOLS.items()}
        names = [inv[int(c)] if int(c) in inv else "eden:" + by_id[int(c)] for c in cells]
        pl.append("    [" + ",\n     ".join(", ".join(jstr(n) for n in names[i:i + 7]) for i in range(0, len(names), 7)) + "]")
    L += [",\n".join(pl), "  ]", "}"]
    with open(out_path, "w", newline="\n") as f:
        f.write("\n".join(L) + "\n")
    print("wrote %s" % out_path)
    return 0


# ---------------------------------------------------------------------------------------------

def read_pack(d):
    p = os.path.join(d, "pack.json")
    if not os.path.isfile(p):
        return None, ["%s: pack.json not found in %s" % (os.path.basename(os.path.normpath(d)), d)]
    with open(p, "rb") as f:
        return f.read(), None


STOCK_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "media", "packs", "eden")


def is_stock_dir(d, force):
    return force or os.path.basename(os.path.normpath(d)) == "eden"


def cmd_validate(d, force_stock=False, stock_dir=STOCK_DIR):
    data, e = read_pack(d)
    if e:
        print(e[0])
        return 1
    if not is_stock_dir(d, force_stock):
        pk = compile_mod(data, d, os.path.basename(os.path.normpath(d)), load_stock_for_mods(stock_dir))
        for line in pk.lines:
            print(line)
        if not pk.refused:
            print("%s: %s (%d of %d blocks, %d tiles)" % ("OK" if len(pk.blocks) == pk.blocks_in_file else "LOADS",
                                                         d, len(pk.blocks), pk.blocks_in_file, len(pk.tiles)))
        return 0 if not pk.refused and len(pk.blocks) == pk.blocks_in_file else 1
    st, errs = compile_pack(data)
    for line in errs:
        print(line)
    if st:
        print("OK: %s (%d stock blocks, %d sound sets)" % (d, STOCK_IDS, len(st.sets)))
    return 0 if st else 1


def extract_mods(path):
    """The text between `[content-dump] mods BEGIN`/`END` in a log."""
    with open(path, "rb") as f:
        text = f.read().decode("utf-8", "replace")
    a, b = text.find("[content-dump] mods BEGIN\n"), text.find("[content-dump] mods END")
    if a < 0 or b < 0:
        raise SystemExit("%s: no [content-dump] mods BEGIN/END" % path)
    return text[a + len("[content-dump] mods BEGIN\n"):b]


def cmd_compile_mods(dirs, check, stock_dir=STOCK_DIR):
    text = load_mods(dirs, load_stock_for_mods(stock_dir))
    if not check:
        sys.stdout.write(text)
        return 0
    want = extract_mods(check)
    if want == text:
        print("OK: %s compile to the mods section of %s (%d bytes)" % (" ".join(dirs), check, len(text)))
        return 0
    import difflib
    print("FAIL: %s do not compile to the mods section of %s" % (" ".join(dirs), check))
    for line in difflib.unified_diff(want.splitlines(), text.splitlines(), check, "pack.py compile", lineterm="", n=1):
        print(line)
    return 1


def cmd_compile(d, check):
    data, e = read_pack(d)
    if e:
        print(e[0])
        return 1
    st, errs = compile_pack(data)
    if not st:
        for line in errs:
            print(line)
        return 1
    text = dump_text(st)
    if not check:
        sys.stdout.write(text)
        return 0
    want = extract_dump(check)
    if want == text:
        print("OK: %s compiles to %s (%d bytes)" % (d, check, len(text)))
        return 0
    import difflib
    print("FAIL: %s does not compile to %s" % (d, check))
    for i, line in enumerate(difflib.unified_diff(want.splitlines(), text.splitlines(), check, "pack.py compile", lineterm="", n=1)):
        if i >= 80:
            print("... (diff cut at 80 lines)")
            break
        print(line)
    return 1


def read_expect(path):
    """expect.txt: the lines, in order; `#` comments; a mod case's `= refused` or `= blocks N` verdict."""
    want, verdict = [], None
    with open(path) as f:
        for l in f:
            l = l.rstrip("\r\n")
            if not l.strip() or l.startswith("#"):
                continue
            if l.startswith("= "):
                verdict = l[2:]
            else:
                want.append(l)
    return want, verdict


def cmd_fixtures(root, stock_dir=STOCK_DIR):
    bad = n = 0
    stock = None
    for case in sorted(os.listdir(root)):
        d = os.path.join(root, case)
        exp = os.path.join(d, "expect.txt")
        if not os.path.isfile(exp):
            continue
        n += 1
        want, verdict = read_expect(exp)
        data, e = read_pack(d)
        if case.startswith("mod-"):
            # A mod pack: the lines, and how much of it loads (a bad block is skipped, not the pack).
            if stock is None:
                stock = load_stock_for_mods(stock_dir)
            pk = compile_mod(data, d, case, stock) if not e else None
            errs = pk.lines if pk else e
            got = "refused" if (pk is None or pk.refused) else "blocks %d" % len(pk.blocks)
            failed = errs != want or got != verdict
            why = "the lines differ from expect.txt" if errs != want else "verdict %s, expect.txt says %s" % (got, verdict)
        else:
            st, errs = (None, e) if e else compile_pack(data)
            failed = st is not None or errs != want
            why = "accepted" if st else "the refusal differs from expect.txt"
        if failed:
            bad += 1
            print("FAIL %s: %s" % (case, why))
            for line in errs:
                print("    got:  " + line)
            for line in want:
                print("    want: " + line)
        else:
            print("ok   %s: %s" % (case, want[0] if want else "(refused)"))
    print("[pack.py] %d fixture(s), %d failure(s) -> %s" % (n, bad, "FAILURES" if bad or not n else "ALL PASS"))
    return 1 if bad or not n else 0


def main(argv):
    if len(argv) >= 3 and argv[1] == "validate":
        return cmd_validate(argv[2], "--stock" in argv)
    if len(argv) >= 3 and argv[1] == "compile":
        check = argv[argv.index("--check") + 1] if "--check" in argv else None
        dirs = [a for i, a in enumerate(argv[2:], 2) if not a.startswith("--") and argv[i - 1] != "--check"]
        if len(dirs) == 1 and is_stock_dir(dirs[0], "--stock" in argv):
            return cmd_compile(dirs[0], check)
        return cmd_compile_mods(dirs, check)
    if len(argv) >= 5 and argv[1] == "extract" and argv[3] == "-o":
        return cmd_extract(argv[2], argv[4])
    if len(argv) >= 3 and argv[1] == "fixtures":
        return cmd_fixtures(argv[2])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
