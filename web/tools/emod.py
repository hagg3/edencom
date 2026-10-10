#!/usr/bin/env python3
"""emod.py -- reference reader/writer for the `.emod` world container (WORKING/ROADMAP.md Stage S, row S.1).

The normative spec is docs/emod-file-format.md; where this file and the spec disagree, the spec wins
and this file has a bug. Independent of engine code on purpose: it is what S.3's EdenWorldStore is
checked against, so it must not share that code's mistakes.

Parsing of `.eden` mirrors FileManager::readDirectory / deriveColumnSpans exactly (last row wins for a
duplicate key, interior gate-failing rows dropped, the contiguous gate-failing run at the end kept
verbatim as the sign trailer up to 1 MiB, creature slots derived from the gap, short spans derived
from the next-higher offset and zero-filled). Needs `pip install zstandard` (pinned 0.25.0, which
bundles libzstd 1.5.7 -- the version S.0(c) measured and S.2 vendors); zlib is stdlib.

  python3 emod.py convert   WORLD.eden [-o OUT.emod] [--codec zstd|zlib|none] [--epoch N]
  python3 emod.py convert --stream WORLD.eden|WORLD.eden.gz|- [-o OUT.emod] [--name SRC.eden] [--epoch N] [--chunk B]
                                                          forward-only (S.3): the reference for the engine's
                                                          EdenEmodConverter; byte-identical to `convert`
  python3 emod.py bake-default Eden.eden [-o Eden.emod] [--level 19] [--epoch N]
                                                          S.6: the bundled RLE map -> Eden.emod (RLE decoded,
                                                          un-transposed to CC(x,z,y), zstd-19, file-offset order)
  python3 emod.py verify-default Eden.eden Eden.emod      S.6: every column of the .emod == an independent RLE decode
  python3 emod.py digest    WORLD.emod                    SHA-256 of the live state (what EdenWorldStore checks)
  python3 emod.py export    WORLD.emod [-o OUT.eden] [--force]
  python3 emod.py verify    WORLD.emod [--no-crc]          exit 0 clean, 3 clean + uncommitted tail, 1 damaged/invalid
  python3 emod.py dump      WORLD.emod [--records]
  python3 emod.py trailer   WORLD.eden|WORLD.emod [--records] [--check] [--diff OTHER]
                                                          D.3a: the trailer's sections; --check = parse ->
                                                          serialise byte-identical; --diff = per-record and
                                                          per-column differences against another world
  python3 emod.py roundtrip WORLD.eden [--work DIR] [--keep] [--no-determinism]
  python3 emod.py measure-order WORLD.eden [--stride N]   native vs transposed band order under zstd-3
  python3 emod.py fixtures  DIR                           write the synthetic .eden fixtures
  python3 emod.py fixtures-pack DIR                       S.3's checked-in pack + expected.txt (web/tools/fixtures/emod)
  python3 emod.py selftest  [--work DIR]                  fixtures round trip + corrupt gate
  python3 emod.py corrupt-gate [--no-crc] [--work DIR]    (also spelled --corrupt-gate)

--epoch N   write N as both FileHeader.created_unix and PROVENANCE.source_mtime instead of the source's
            mtime, so the output is a pure function of the source bytes (for cross-machine fixtures).
--name      the source file name PROVENANCE records for --stream (default: the input's basename minus .gz;
            'stdin.eden' for '-'). `convert X.eden --epoch N` == `convert --stream X.eden.gz --epoch N`.
"""
import argparse, hashlib, mmap, os, random, struct, sys, time, types, zlib

try:
    import zstandard
except ImportError:
    zstandard = None

# ------------------------------------------------------------------ .eden constants
EDEN_HEADER = 192
BAND = 8192                      # 4096 type bytes + 4096 paint bytes, CC(x,z,y) order
DIR_ROW = 16                     # ColumnIndex {i32 x, i32 z, u64 chunk_offset}
ENT = 60                         # EntityData
MAX_SLOTS = 400                  # MAX_CREATURES_SAVED_MAX
TRAILER_MAX = 1 << 20            # DIR_TRAILER_MAX
SPILL_BANDS = 16                 # StreamConverter's fixed spill grid (S.3b)
SPILL_GRID = SPILL_BANDS * BAND

# ------------------------------------------------------------------ .emod constants
MAGIC = b'EMODWLD\0'
FMT_VERSION = 1
FILE_HEADER = 64
TAG = b'EmR1'
REC_HEADER = 32
MAX_PAYLOAD = 16 << 20
T_COLUMN, T_WORLD_HEADER, T_CREATURES, T_SIGN_TRAILER, T_PROVENANCE, T_SUMMARY, T_COMMIT, T_INDEX = range(1, 9)
TYPE_NAMES = {1: 'COLUMN', 2: 'WORLD_HEADER', 3: 'CREATURES', 4: 'SIGN_TRAILER', 5: 'PROVENANCE',
              6: 'SUMMARY', 7: 'COMMIT', 8: 'INDEX'}
C_NONE, C_ZSTD, C_ZLIB = 0, 1, 2
CODEC_NAMES = {0: 'none', 1: 'zstd', 2: 'zlib'}
FH_FMT = '<8sHHIQ36sI'           # 64 B
RH28_FMT = '<4sBBHiiIII'         # first 28 B of a record header; crc32 follows
COMMIT_FMT = '<IIIIQ'            # seq, count, crc_of_crcs, reserved, prev_commit_offset (24 B)
SUMMARY_FMT = '<IIHHI'           # live_columns, next_ord, band_or, reserved, reserved (16 B)

# PROVENANCE flags: each names one reason an export cannot be byte-identical to the source.
PF_NONCANONICAL_LAYOUT = 1       # columns not packed from offset 192 at full stride, or dead bytes
PF_SHORT_SPANS = 2
PF_DROPPED_ROWS = 4              # interior gate-failing directory rows (the engine drops them too)
PF_DUPLICATE_KEYS = 8
PF_DUPLICATE_OFFSETS = 16
PF_PARTIAL_DIR_ROW = 32          # directory region not a multiple of 16 bytes
PF_CREATURE_GAP_INVALID = 64
PF_TRAILER_DROPPED = 128         # gate-failing tail over 1 MiB (the engine drops it too)
PF_UPGRADED_256Z = 256           # S.5e: the engine promoted a 64z source to 256z (emod.py never sets it)
PF_NAMES = {1: 'non-canonical layout / dead bytes', 2: 'short spans', 4: 'dropped interior rows',
            8: 'duplicate keys', 16: 'duplicate offsets', 32: 'partial directory row',
            64: 'creature gap invalid', 128: 'sign trailer over 1 MiB dropped',
            256: 'upgraded 64z -> 256z at conversion'}


def die(msg, code=1):
    print('emod.py: ' + msg, file=sys.stderr)
    sys.exit(code)


def need_zstd():
    if zstandard is None:
        die('python zstandard is not installed (pip install zstandard==0.25.0)')


def crc32(*parts):
    c = 0
    for p in parts:
        c = zlib.crc32(p, c)
    return c & 0xffffffff


def popcount(m):
    return bin(m).count('1')


def pad8(n):
    return (-n) % 8


# ================================================================== .eden reader
def detect_bands(version, dir_off, offsets):
    """S.3b: a .eden's height (4 or 16 bands) from the FILE -- the header `version` is not a height
    marker (the 2026 game stamps v2 on some 256z worlds). VuencEdit's rule
    (`detect_chunk_size_by_creature_gap`): version >= 5 -> 16; otherwise the stride (131,072 first,
    then 32,768) for which dir_off - (highest live offset + stride) is 0 or a whole number of 60-byte
    slots <= 24,000; when neither is, the smallest gap between distinct live offsets (>= 131,072 ->
    16); fewer than two columns -> 4. `offsets` are the live rows' offsets. The engine's twin is
    emod::eden_detect_bands (EdenWorldStore.cpp)."""
    if version >= 5:
        return 16
    if not offsets:
        return 4
    mo = max(offsets)
    for bands in (16, 4):
        gap = dir_off - (mo + bands * BAND)
        if gap >= 0 and gap % ENT == 0 and gap <= ENT * MAX_SLOTS:
            return bands
    s = sorted(set(offsets))
    gaps = [b - a for a, b in zip(s, s[1:])]
    return 16 if gaps and min(gaps) >= 16 * BAND else 4


class Layout:
    """The engine's reading of a .eden from its 192-byte header, its total size and its directory
    region (`directory_offset`..EOF): live columns, spans, creature slots, trailer, PROVENANCE facts.
    Shared by the random-access reader (Eden) and the forward-only StreamConverter, so the two
    cannot drift: it never touches the column region."""

    def __init__(self, path, h, size, raw):
        self.path, self.header, self.size = path, h, size
        self.version = struct.unpack_from('<i', h, 92)[0]
        self.dir_off = struct.unpack_from('<Q', h, 32)[0]
        self.flags = 0
        self.notes = []
        nrows = len(raw) // DIR_ROW
        self.dir_rows = nrows
        if len(raw) % DIR_ROW:
            self.flags |= PF_PARTIAL_DIR_ROW
            self.notes.append('%d stray bytes after the last whole directory row' % (len(raw) % DIR_ROW))
        live = {}                                   # key -> (x, z, off, row index)
        pending = []
        dropped = dups = 0
        for i in range(nrows):
            x, z, o = struct.unpack_from('<iiQ', raw, i * DIR_ROW)
            if 0 <= x < 32768 and 0 <= z < 32768 and ((x << 15) + z) != 0:
                dropped += len(pending)
                pending = []
                k = (x, z)
                if k in live:
                    dups += 1
                live[k] = (x, z, o, i)
            else:
                pending.append(raw[i * DIR_ROW:(i + 1) * DIR_ROW])
        self.trailer = b''.join(pending)
        if len(self.trailer) > TRAILER_MAX:
            self.flags |= PF_TRAILER_DROPPED
            self.notes.append('%d B of trailing unaddressable rows exceed 1 MiB: dropped, as the engine does' % len(self.trailer))
            self.trailer = b''
        if dropped:
            self.flags |= PF_DROPPED_ROWS
            self.notes.append('%d interior gate-failing directory rows dropped' % dropped)
        if dups:
            self.flags |= PF_DUPLICATE_KEYS
            self.notes.append('%d duplicate directory keys (last row wins)' % dups)
        ents = sorted(live.values(), key=lambda e: e[3])
        self.dir_order = [(e[0], e[1]) for e in ents]
        offsets = sorted(e[2] for e in ents)
        if len(set(offsets)) != len(offsets):
            self.flags |= PF_DUPLICATE_OFFSETS
            self.notes.append('%d directory rows share an offset' % (len(offsets) - len(set(offsets))))
        n = len(offsets)
        self.bands = detect_bands(self.version, self.dir_off, offsets)
        self.col = self.bands * BAND
        default_slots = 400 if self.bands == 16 else 200
        slots = default_slots
        if n:
            last_end = offsets[-1] + self.col
            gap = self.dir_off - last_end if self.dir_off >= last_end else None
            if gap is not None and gap % ENT == 0 and gap // ENT <= MAX_SLOTS:
                slots = gap // ENT
            else:
                self.flags |= PF_CREATURE_GAP_INVALID
                self.notes.append('creature gap is not a whole number of slots: assumed %d, as the engine does' % slots)
        self.slots = slots
        self.block_end = self.dir_off - ENT * slots
        if self.block_end < EDEN_HEADER:
            die('%s: creature block (%d slots) would start inside the header' % (path, slots))
        # spans: a port of deriveColumnSpans' assignSpan (binary search, then next offset)
        self.cols = []                               # (x, z, off, span) in ord order
        short = []
        for x, z, o, _ in ents:
            lo, hi, at = 0, n - 1, -1
            while lo <= hi:
                mid = (lo + hi) // 2
                if offsets[mid] == o:
                    at = mid
                    break
                if offsets[mid] < o:
                    lo = mid + 1
                else:
                    hi = mid - 1
            span = self.col
            if at >= 0:
                nxt = offsets[at + 1] if at + 1 < n else self.block_end
                if nxt > o and nxt - o < self.col:
                    span = nxt - o
            if span < self.col:
                short.append((x, z, span))
            self.cols.append((x, z, o, span))
        self.cols.sort(key=lambda c: c[2])           # ord = rank by file offset (stable on ties)
        self.short = short
        if short:
            self.flags |= PF_SHORT_SPANS
            self.notes.append('%d short column spans (zero-filled, exported at full stride)' % len(short))
        canon = [EDEN_HEADER + i * self.col for i in range(n)]
        referenced = sum(min(c[3], max(0, self.size - c[2])) for c in self.cols)
        self.dead_bytes = max(0, (self.block_end - EDEN_HEADER) - referenced)
        if offsets != canon or self.dir_off != EDEN_HEADER + n * self.col + ENT * slots:
            self.flags |= PF_NONCANONICAL_LAYOUT
            self.notes.append('columns are not packed from offset 192 at full stride (%d dead bytes)' % self.dead_bytes)
        hs = hashlib.sha256()
        hs.update(h)
        hs.update(raw)
        self.source_hash = hs.digest()

    def identical_expected(self):
        return self.flags == 0


def check_eden_header(path, h, size=None):
    """The header checks both readers make before trusting directory_offset."""
    if len(h) != EDEN_HEADER:
        die('%s: shorter than a 192-byte header' % path)
    version = struct.unpack_from('<i', h, 92)[0]
    if not (1 <= version <= 1000):
        die('%s: legacy 1.x file (version field %d) -- open it in the game once to convert it' % (path, version))
    if version > 6:
        die('%s: version %d is newer than any known .eden (the engine refuses > 6 too)' % (path, version))
    dir_off = struct.unpack_from('<Q', h, 32)[0]
    if dir_off < EDEN_HEADER or (size is not None and dir_off > size):
        die('%s: directory offset %d outside the file' % (path, dir_off))
    return version, dir_off


class Eden(Layout):
    """A parsed .eden, with the engine's view of every column (see the module docstring)."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        size = os.fstat(self.f.fileno()).st_size
        self.mtime = int(os.stat(path).st_mtime)
        h = self.f.read(EDEN_HEADER)
        _, dir_off = check_eden_header(path, h, size)
        self.f.seek(dir_off)
        Layout.__init__(self, path, h, size, self.f.read(size - dir_off))
        self.f.seek(self.block_end)
        self.creatures = self.f.read(ENT * self.slots)
        if len(self.creatures) != ENT * self.slots:
            die('%s: creature block runs past EOF' % path)

    def column(self, c):
        """The engine's view of one column: `span` bytes from the record, zero-filled to full stride."""
        x, z, o, span = c
        self.f.seek(o)
        b = self.f.read(span)
        return b + bytes(self.col - len(b))


def split_bands(col, bands):
    """(band_mask, payload): a band is stored iff any of its 8,192 bytes is non-zero (types OR paint)."""
    mask = 0
    parts = []
    for b in range(bands):
        seg = col[b * BAND:(b + 1) * BAND]
        if seg.count(0) != BAND:
            mask |= 1 << b
            parts.append(seg)
    return mask, b''.join(parts)


def join_bands(mask, payload, bands):
    out = bytearray(bands * BAND)
    p = 0
    for b in range(bands):
        if mask >> b & 1:
            out[b * BAND:(b + 1) * BAND] = payload[p:p + BAND]
            p += BAND
    return bytes(out)


# ================================================================== .emod writer
class Writer:
    """Appends one batch. The caller positions `f` at the batch start (an 8-aligned offset)."""

    def __init__(self, f, seq, prev_commit_off, codec=C_ZSTD, level=3):
        self.f, self.seq, self.prev = f, seq, prev_commit_off
        self.codec, self.level = codec, level
        self.crcs = []
        if codec == C_ZSTD:
            need_zstd()
            self.zc = zstandard.ZstdCompressor(level=level, write_checksum=True, write_content_size=True,
                                               write_dict_id=False)

    def encode(self, data, compress):
        if not data or not compress or self.codec == C_NONE:
            return C_NONE, data
        if self.codec == C_ZSTD:
            return C_ZSTD, self.zc.compress(data)
        return C_ZLIB, zlib.compress(data, 6)

    def _emit(self, rtype, codec, data, mask, x, z, aux):
        if len(data) > MAX_PAYLOAD:
            die('record payload %d B exceeds the 16 MiB limit' % len(data))
        off = self.f.tell()
        assert off % 8 == 0
        h28 = struct.pack(RH28_FMT, TAG, rtype, codec, mask, x, z, aux, len(data), self.seq)
        c = crc32(h28, data)
        self.f.write(h28 + struct.pack('<I', c) + data + bytes(pad8(REC_HEADER + len(data))))
        return off, c

    def rec(self, rtype, payload, mask=0, x=0, z=0, aux=None, compress=True):
        codec, data = self.encode(payload, compress)
        off, c = self._emit(rtype, codec, data, mask, x, z, len(payload) if aux is None else aux)
        self.crcs.append(c)
        return off

    def rec_encoded(self, rtype, codec, data, mask, x, z, aux):
        off, c = self._emit(rtype, codec, data, mask, x, z, aux)
        self.crcs.append(c)
        return off

    def commit(self):
        p = struct.pack(COMMIT_FMT, self.seq, len(self.crcs),
                        crc32(b''.join(struct.pack('<I', c) for c in self.crcs)), 0, self.prev)
        off, _ = self._emit(T_COMMIT, C_NONE, p, 0, 0, 0, len(p))
        return off


def file_header(bands, created):
    body = struct.pack('<8sHHIQ36s', MAGIC, FMT_VERSION, bands, 0, created, bytes(36))
    return body + struct.pack('<I', crc32(body))


def summary_payload(live_cols, next_ord, band_or):
    return struct.pack(SUMMARY_FMT, live_cols, next_ord, band_or, 0, 0)


def provenance_payload(e, mtime):
    name = os.path.basename(e.path).encode('utf8')
    out = [struct.pack('<IIQq32sIIQI', 1, e.flags, e.size, mtime, e.source_hash, e.version, e.dir_rows,
                       e.dead_bytes, e.slots),
           struct.pack('<I', len(name)), name,
           struct.pack('<I', len(e.short))] + [struct.pack('<iiI', x, z, s) for x, z, s in e.short]
    out.append(struct.pack('<I', len(e.dir_order)))
    out += [struct.pack('<ii', x, z) for x, z in e.dir_order]
    return b''.join(out)


def parse_provenance(p):
    (ver, flags, size, mtime, h, sver, rows, dead, slots) = struct.unpack_from('<IIQq32sIIQI', p, 0)
    o = struct.calcsize('<IIQq32sIIQI')
    nl = struct.unpack_from('<I', p, o)[0]; o += 4
    name = p[o:o + nl].decode('utf8', 'replace'); o += nl
    ns = struct.unpack_from('<I', p, o)[0]; o += 4
    short = [struct.unpack_from('<iiI', p, o + 12 * i) for i in range(ns)]; o += 12 * ns
    nd = struct.unpack_from('<I', p, o)[0]; o += 4
    order = [struct.unpack_from('<ii', p, o + 8 * i) for i in range(nd)]; o += 8 * nd
    if o != len(p):
        raise ValueError('PROVENANCE has %d trailing bytes' % (len(p) - o))
    return dict(version=ver, flags=flags, size=size, mtime=mtime, hash=h, source_version=sver, dir_rows=rows,
                dead_bytes=dead, slots=slots, name=name, short=short, dir_order=order)


def write_converted(out, L, creatures, created, codec, column, quiet_label=None, level=3):
    """The one conversion batch (spec "Conversion from .eden"), shared by convert and the stream
    converter. `column(ordn, c, w)` returns (mask, payload, sha256 of the engine view) for each live
    column in offset order. Writes `out`.converting, re-reads and compares, fsyncs, renames."""
    tmp = out + '.converting'
    digests = {}
    band_or = 0
    with open(tmp, 'wb') as f:
        f.write(file_header(L.bands, created))
        w = Writer(f, 1, 0, codec, level)
        w.rec(T_WORLD_HEADER, L.header, compress=False)
        w.rec(T_CREATURES, creatures)
        if L.trailer:
            w.rec(T_SIGN_TRAILER, L.trailer)
        w.rec(T_PROVENANCE, provenance_payload(L, created))
        for ordn, c in enumerate(L.cols):
            mask, payload, digest = column(ordn, c, w)
            digests[(c[0], c[1])] = digest
            band_or |= mask
            if isinstance(payload, tuple):                 # (codec, stored bytes), already encoded
                w.rec_encoded(T_COLUMN, payload[0], payload[1], mask=mask, x=c[0], z=c[1], aux=ordn)
            else:
                w.rec(T_COLUMN, payload, mask=mask, x=c[0], z=c[1], aux=ordn)
        w.rec(T_SUMMARY, summary_payload(len(L.cols), len(L.cols), band_or), compress=False)
        w.commit()
        f.flush()
        os.fsync(f.fileno())
    # convert-on-open step 3: re-read every record and compare it to the source before the rename
    st = open_emod(tmp)
    if st.problems() or st.torn or st.tail_bytes:
        die('freshly written %s does not verify: %s' % (tmp, '; '.join(st.problems()) or 'torn/tail'))
    for k, r in st.columns().items():
        if hashlib.sha256(join_bands(r.mask, st.decode(r), st.bands)).digest() != digests.get(k):
            die('freshly written column %s does not decode to the source bytes' % (k,))
    if len(st.columns()) != len(digests):
        die('freshly written file has %d columns, source %d' % (len(st.columns()), len(digests)))
    st.close()
    os.replace(tmp, out)


def convert(src, out, codec=C_ZSTD, epoch=None, quiet=False):
    e = Eden(src)
    created = e.mtime if epoch is None else epoch
    t0 = time.time()

    def column(ordn, c, w):
        data = e.column(c)
        mask, payload = split_bands(data, e.bands)
        return mask, payload, hashlib.sha256(data).digest()

    write_converted(out, e, e.creatures, created, codec, column)
    if not quiet:
        print('converted %s -> %s: %d columns, %d bands, %d creature slots, trailer %d B, %.1f MB -> %.2f MB (%.1fs)' % (
            src, out, len(e.cols), e.bands, e.slots, len(e.trailer), e.size / 1048576.0, os.path.getsize(out) / 1048576.0,
            time.time() - t0))
        print('  export byte-identical to source: %s' % ('expected' if e.identical_expected() else
                                                         'NO -- ' + '; '.join(e.notes)))
    return e


# ================================================================== streaming conversion
class StreamConverter:
    """Forward-only `.eden` -> `.emod` (plan section 4 "Downloads and imports"; S.3 gate 6). The
    reference for the engine's EdenEmodConverter: same algorithm, same output, byte for byte.

    The source arrives as a byte stream and is never seeked. Directory facts (which offsets are live,
    their keys, the creature slot count, the trailer) only exist at EOF, so:
      1. the first 192 bytes are the header; it gives `directory_offset`;
      2. [192, directory_offset) is cut into SLOTS of one fixed grid, 131,072 B (16 bands), at
         192 + k*131072 -- whatever the world's height, which is only known at EOF (S.3b: the header
         `version` does not decide it). Each slot is split
         into bands and encoded exactly as a COLUMN payload would be, then appended to a SPILL file
         (`<out>.spill`) with an index entry {spill offset, mask, codec, stored length, slot length,
         sha256 of the slot}. All-zero slots spill as mask 0, i.e. nothing;
      3. [directory_offset, EOF) is the directory (and any sign trailer), held in RAM;
      4. at EOF the directory is parsed by the same Layout as `convert`. A live column on a slot
         boundary with a full-stride span inside the region IS that slot (256z only): its spilled payload is
         copied; a 64z column is a quarter of a slot and is rebuilt. Anything else (a short span, an off-grid offset, an offset past the region) is
         rebuilt byte-exactly from the spill (+ header / directory / zeros, as `Eden.column` reads
         it) and re-encoded. The creature block is read back the same way.
    RAM: one slot buffer + the directory + O(slots) index entries -- never the world. Disk: the spill
    (about the size of the .emod) + the .converting file."""

    def __init__(self, out, name, created, codec=C_ZSTD):
        self.out, self.name, self.created, self.codec = out, name, created, codec
        self.spill_path = out + '.spill'
        self.spill = open(self.spill_path, 'w+b')
        self.enc = Writer(None, 0, 0, codec)              # only its encoder is used
        self.hdr = bytearray()
        self.dir = bytearray()
        self.slot = bytearray()
        self.pos = 0                                       # source bytes consumed
        self.index = []                                    # per slot: (spill_off, mask, codec, stored, slot_len, sha)
        self.dir_off = None
        self.col = None                                    # the detected stride, set in finish()
        self.peak_held = 0
        self.cache = {}

    def feed(self, data):
        mv = memoryview(data)
        while len(mv):
            if self.dir_off is None:
                take = min(len(mv), EDEN_HEADER - len(self.hdr))
                self.hdr += mv[:take]
                mv = mv[take:]
                self.pos += take
                if len(self.hdr) == EDEN_HEADER:
                    _, self.dir_off = check_eden_header(self.name, bytes(self.hdr))
                continue
            if self.pos < self.dir_off:
                take = min(len(mv), self.dir_off - self.pos, SPILL_GRID - len(self.slot))
                self.slot += mv[:take]
                mv = mv[take:]
                self.pos += take
                if len(self.slot) == SPILL_GRID or self.pos == self.dir_off:
                    self._spill_slot()
                continue
            self.dir += mv
            self.pos += len(mv)
            mv = mv[len(mv):]
        self.peak_held = max(self.peak_held, len(self.slot) + len(self.dir))

    def _spill_slot(self):
        n = len(self.slot)
        full = bytes(self.slot) + bytes(SPILL_GRID - n)     # a partial last slot is zero-padded to split
        mask, payload = split_bands(full, SPILL_BANDS)
        codec, data = self.enc.encode(payload, True)
        off = self.spill.tell()
        self.spill.write(data)
        self.index.append((off, mask, codec, len(data), n, hashlib.sha256(bytes(self.slot)).digest()))
        self.slot = bytearray()

    def _slot_raw(self, k):
        if k in self.cache:
            return self.cache[k]
        off, mask, codec, stored, n, _ = self.index[k]
        self.spill.seek(off)
        data = self.spill.read(stored)
        payload = data if codec == C_NONE else Store.decode_payload(codec, data, popcount(mask) * BAND)
        raw = join_bands(mask, payload, SPILL_BANDS)[:n]
        if hashlib.sha256(raw).digest() != self.index[k][5]:     # else the rebuild check is self-referential
            die('%s: slot %d does not read back from the spill as fed' % (self.name, k))
        if len(self.cache) >= 2:
            self.cache.pop(next(iter(self.cache)))
        self.cache[k] = raw
        return raw

    def read_source(self, o, n):
        """Bytes [o, o+n) of the source as a file read sees them; short at EOF (like f.read)."""
        out = bytearray()
        end = min(o + n, self.pos)
        while o < end:
            if o < EDEN_HEADER:
                take = min(end, EDEN_HEADER) - o
                out += self.hdr[o:o + take]
            elif o < self.dir_off:
                k = (o - EDEN_HEADER) // SPILL_GRID
                base = EDEN_HEADER + k * SPILL_GRID
                take = min(end, self.dir_off, base + SPILL_GRID) - o
                out += self._slot_raw(k)[o - base:o - base + take]
            else:
                take = end - o
                out += self.dir[o - self.dir_off:o - self.dir_off + take]
            o += take
        return bytes(out)

    def finish(self, quiet=False):
        if self.dir_off is None:
            die('%s: shorter than a 192-byte header' % self.name)
        if self.pos < self.dir_off:
            die('%s: directory offset %d outside the file' % (self.name, self.dir_off))
        if len(self.slot):
            self._spill_slot()
        self.spill.flush()
        L = Layout(self.name, bytes(self.hdr), self.pos, bytes(self.dir))
        self.bands, self.col = L.bands, L.col
        creatures = self.read_source(L.block_end, ENT * L.slots)
        if len(creatures) != ENT * L.slots:
            die('%s: creature block runs past EOF' % self.name)
        copied = [0, 0]

        def column(ordn, c, w):
            x, z, o, span = c
            k, rem = divmod(o - EDEN_HEADER, SPILL_GRID)
            if (self.col == SPILL_GRID and o >= EDEN_HEADER and rem == 0 and span == self.col
                    and k < len(self.index) and self.index[k][4] == SPILL_GRID):
                off, mask, codec, stored, n, sha = self.index[k]
                self.spill.seek(off)
                copied[0] += 1
                return mask, (codec, self.spill.read(stored)), sha
            data = self.read_source(o, span)
            data += bytes(self.col - len(data))
            mask, payload = split_bands(data, self.bands)
            copied[1] += 1
            return mask, payload, hashlib.sha256(data).digest()

        write_converted(self.out, L, creatures, self.created, self.codec, column)
        self.spill.close()
        os.remove(self.spill_path)
        if not quiet:
            print('stream-converted %s -> %s: %d columns (%d copied from the spill, %d rebuilt), %d bands, %d creature slots, '
                  'trailer %d B, %.1f MB in, spill %.2f MB, peak held %d B + %d index entries' % (
                      self.name, self.out, len(L.cols), copied[0], copied[1], L.bands, L.slots, len(L.trailer),
                      self.pos / 1048576.0, sum(i[3] for i in self.index) / 1048576.0, self.peak_held, len(self.index)))
        return L


def gunzip_chunks(f, chunk=1 << 20):
    """Yield the bytes of `f` in pieces of at most `chunk`: inflated if it starts with the gzip magic
    (multi-member aware), else raw. Inflate output is bounded too -- a .eden is ~99% zeros, so 1 MB
    of gzip can be a gigabyte of output."""
    first = f.read(max(2, chunk))
    if first[:2] != b'\x1f\x8b':
        while first:
            yield first
            first = f.read(chunk)
        return
    d = zlib.decompressobj(16 + zlib.MAX_WBITS)
    buf = first
    first = None
    while True:
        if not buf:
            buf = f.read(chunk)
            if not buf:
                break
        out = d.decompress(buf, chunk)
        buf = d.unconsumed_tail
        if out:
            yield out
        if d.eof:
            buf = d.unused_data
            d = zlib.decompressobj(16 + zlib.MAX_WBITS)
            if not buf:
                buf = f.read(chunk)
                if not buf:
                    break
    tail = d.flush()
    if tail:
        yield tail


def convert_stream(src, out, codec=C_ZSTD, epoch=None, name=None, quiet=False, chunk=1 << 20):
    """`emod.py convert --stream`: src is a path or '-' (stdin); .eden or gzip'd .eden, read forward only."""
    t0 = time.time()
    if src == '-':
        f = sys.stdin.buffer
        mtime = int(time.time())
    else:
        f = open(src, 'rb')
        mtime = int(os.stat(src).st_mtime)
    if name is None:
        name = 'stdin.eden' if src == '-' else os.path.basename(src)
        if name.endswith('.gz'):
            name = name[:-3]
    sc = StreamConverter(out, name, mtime if epoch is None else epoch, codec)
    for piece in gunzip_chunks(f, chunk):
        sc.feed(piece)
    L = sc.finish(quiet)
    if not quiet:
        print('  %.1fs' % (time.time() - t0))
    return L


# ================================================================== .emod reader
class Rec:
    __slots__ = ('off', 'type', 'codec', 'mask', 'x', 'z', 'aux', 'plen', 'seq', 'crc', 'end', 'pad_ok')

    @property
    def poff(self):
        return self.off + REC_HEADER


class Store:
    """Scan + commit evaluation, per docs/emod-file-format.md "Reading"."""

    def __init__(self, path, check_crc=True):
        self.path, self.check_crc = path, check_crc
        self.fobj = open(path, 'rb')
        self.size = os.fstat(self.fobj.fileno()).st_size
        self.buf = mmap.mmap(self.fobj.fileno(), 0, access=mmap.ACCESS_READ) if self.size else b''
        self.errors = []            # fatal / invalid
        self.damage = []            # media damage inside committed data (sets the damaged flag)
        self.warnings = []
        self.recs, self.spans, self.commits = [], [], []
        self.history = {}           # key -> [Rec,...] applied, in file order
        self.torn = False
        self.tail_bytes = 0
        self.truncate_at = FILE_HEADER
        self.last_commit = None
        self.last_seq = 0
        self.fh_ok = self._file_header()
        if self.fh_ok:
            self._scan()
            self._evaluate()

    def close(self):
        if self.size:
            self.buf.close()
        self.fobj.close()

    # -- file header
    def _file_header(self):
        if self.size < FILE_HEADER:
            self.errors.append('file shorter than the 64-byte file header')
            return False
        magic, ver, bands, flags, created, res, c = struct.unpack_from(FH_FMT, self.buf, 0)
        self.bands, self.created, self.fmt_version = bands, created, ver
        if magic != MAGIC:
            self.errors.append('bad magic %r' % magic)
            return False
        if self.check_crc and crc32(self.buf[0:60]) != c:
            self.errors.append('file header CRC mismatch')
            return False
        if ver != FMT_VERSION:
            self.errors.append('fmt_version %d (this reader knows %d)' % (ver, FMT_VERSION))
            return False
        if bands not in (4, 16):
            self.errors.append('bands %d is neither 4 nor 16' % bands)
            return False
        if flags or res.count(0) != len(res):
            self.errors.append('non-zero flags/reserved in the file header (written by a newer build)')
            return False
        return True

    # -- framing
    def _frame(self, p):
        b, n = self.buf, self.size
        if p + REC_HEADER > n or b[p:p + 4] != TAG:
            return None
        tag, t, codec, mask, x, z, aux, plen, seq = struct.unpack_from(RH28_FMT, b, p)
        if plen > MAX_PAYLOAD:
            return None
        end = p + REC_HEADER + plen + pad8(REC_HEADER + plen)
        if end > n:
            return None
        c = struct.unpack_from('<I', b, p + 28)[0]
        if self.check_crc and crc32(b[p:p + 28], b[p + REC_HEADER:p + REC_HEADER + plen]) != c:
            return None
        r = Rec()
        r.off, r.type, r.codec, r.mask, r.x, r.z, r.aux, r.plen, r.seq, r.crc, r.end = p, t, codec, mask, x, z, aux, plen, seq, c, end
        r.pad_ok = b[p + REC_HEADER + plen:end].count(0) == end - (p + REC_HEADER + plen)
        return r

    def _scan(self):
        p = FILE_HEADER
        while p < self.size:
            r = self._frame(p)
            if r:
                self.recs.append(r)
                p = r.end
                continue
            q = self.buf.find(TAG, p + 1)
            while q != -1 and not (q % 8 == 0 and self._frame(q)):
                q = self.buf.find(TAG, q + 1)
            if q == -1:
                q = self.size
            self.spans.append((p, q))
            p = q

    # -- commits
    def _commit_fields(self, r):
        if r.type != T_COMMIT or r.codec != C_NONE or r.plen != 24 or r.aux != 24 or r.mask or r.x or r.z:
            return None
        f = struct.unpack_from(COMMIT_FMT, self.buf, r.poff)
        if f[0] != r.seq or f[3] != 0:
            return None
        return f

    def _evaluate(self):
        commits = [(r, self._commit_fields(r)) for r in self.recs]
        commits = [(r, f) for r, f in commits if f]
        self.commits = commits
        prev, prev_end, prev_seq = None, FILE_HEADER, None
        for k, (c, (seq, count, crcc, _, prev_off)) in enumerate(commits):
            last = k == len(commits) - 1
            rng = [r for r in self.recs if prev_end <= r.off < c.off]
            spans = [s for s in self.spans if prev_end <= s[0] < c.off]
            batch = [r for r in rng if r.seq == seq]
            orph = [r for r in rng if r.seq != seq]
            # prev_commit_offset must name the previous intact commit -- or point inside this range,
            # which means the commit it names was destroyed (reported below as orphans/spans).
            prev_ok = prev_off == (prev.off if prev else 0) or (prev_end <= prev_off < c.off and (orph or spans))
            ok = (len(batch) == count and prev_ok and
                  (not self.check_crc or crc32(b''.join(struct.pack('<I', r.crc) for r in batch)) == crcc))
            gap = prev_seq is not None and seq != prev_seq + 1
            early = [r for r in orph if r.seq < seq]
            if orph or (spans and ok) or gap:
                self.damage.append('batch seq %d (commit @%d): %d orphan records, %d damaged spans%s' % (
                    seq, c.off, len(orph), len(spans), ', seq gap after %s' % prev_seq if gap else ''))
            if ok or not last:
                if not ok:
                    self.damage.append('batch seq %d (commit @%d) fails its commit check; applying its %d intact records' % (
                        seq, c.off, len(batch)))
                apply = sorted(early + batch, key=lambda r: r.off)
                self.truncate_at = c.end
                self.last_commit, self.last_seq = c, seq
            else:
                self.torn = True
                apply = early
                self.truncate_at = max([r.end for r in early] + [prev_end])
                self.warnings.append('last batch seq %d (commit @%d) is torn: %d of %d records present -- rolled back' % (
                    seq, c.off, len(batch), count))
            for r in apply:
                self.history.setdefault(self.key(r), []).append(r)
            prev, prev_end, prev_seq = c, c.end, seq
        end = commits[-1][0].end if commits else FILE_HEADER
        self.tail_bytes = self.size - end
        if not commits:
            self.errors.append('no committed batch')
        for r in self.recs:
            if not r.pad_ok and r.off < end:
                self.damage.append('non-zero padding after record @%d' % r.off)

    @staticmethod
    def key(r):
        return ('C', r.x, r.z) if r.type == T_COLUMN else ('T', r.type)

    # -- live view
    def live(self, rtype):
        h = self.history.get(('T', rtype))
        return h[-1] if h else None

    def columns(self):
        return {(k[1], k[2]): h[-1] for k, h in self.history.items() if k[0] == 'C'}

    def decode(self, r):
        p = bytes(self.buf[r.poff:r.poff + r.plen])
        if r.type == T_COLUMN:
            exp = popcount(r.mask) * BAND
        else:
            exp = r.aux
        return Store.decode_payload(r.codec, p, exp)

    @staticmethod
    def decode_payload(codec, p, exp):
        if codec == C_NONE:
            if len(p) != exp:
                raise ValueError('raw payload is %d B, expected %d' % (len(p), exp))
            return p
        if exp == 0:
            raise ValueError('compressed record with an empty decoded size')
        if codec == C_ZSTD:
            need_zstd()
            fp = zstandard.get_frame_parameters(p)
            if not fp.has_checksum:
                raise ValueError('zstd frame without a content checksum')
            if fp.content_size != exp:
                raise ValueError('zstd frame content size %s, expected %d' % (fp.content_size, exp))
            d = zstandard.ZstdDecompressor().decompressobj()
            out = d.decompress(p)
            if not d.eof or d.unused_data or len(out) != exp:
                raise ValueError('zstd payload is not exactly one %d-byte frame' % exp)
            return out
        if codec == C_ZLIB:
            d = zlib.decompressobj()
            out = d.decompress(p, exp + 1)
            if not d.eof or d.unused_data or d.unconsumed_tail or len(out) != exp:
                raise ValueError('zlib payload is not exactly one %d-byte stream' % exp)
            return out
        raise ValueError('reserved codec %d' % codec)

    def decode_live(self, key):
        """Newest decodable committed version of `key` (the spec's damaged-record fallback)."""
        h = self.history.get(key, [])
        for i in range(len(h) - 1, -1, -1):
            try:
                return h[i], self.decode(h[i]), len(h) - 1 - i
            except Exception:
                continue
        return None, None, len(h)

    def problems(self):
        return self.errors + self.damage

    # -- full semantic check (verify)
    def verify(self):
        errs = []
        if not self.fh_ok or not self.commits:
            return self.problems()
        for r in self.recs:
            if r.type not in TYPE_NAMES and r.off < self.truncate_at:
                errs.append('record @%d has reserved type %d' % (r.off, r.type))
        wh = self.live(T_WORLD_HEADER)
        if not wh:
            errs.append('no WORLD_HEADER')
        else:
            try:
                h = self.decode(wh)
                if len(h) != EDEN_HEADER:
                    errs.append('WORLD_HEADER is %d B' % len(h))
                else:
                    v = struct.unpack_from('<i', h, 92)[0]
                    if not (1 <= v <= 6) or (v >= 5 and self.bands != 16):   # S.3b: v5/6 => 16, never <=>
                        errs.append('WORLD_HEADER version %d does not match bands=%d' % (v, self.bands))
            except Exception as ex:
                errs.append('WORLD_HEADER @%d: %s' % (wh.off, ex))
            if wh.codec != C_NONE:
                errs.append('WORLD_HEADER must be codec 0')
        cr = self.live(T_CREATURES)
        if not cr:
            errs.append('no CREATURES')
        elif cr.aux % ENT or cr.aux // ENT > MAX_SLOTS:
            errs.append('CREATURES raw length %d is not 0..400 slots of 60 B' % cr.aux)
        st = self.live(T_SIGN_TRAILER)
        if st and (st.aux % DIR_ROW or st.aux > TRAILER_MAX):
            errs.append('SIGN_TRAILER raw length %d is not a multiple of 16 up to 1 MiB' % st.aux)
        for t in (T_CREATURES, T_SIGN_TRAILER, T_PROVENANCE, T_SUMMARY):
            r = self.live(t)
            if r:
                if r.mask or r.x or r.z:
                    errs.append('%s @%d: band_mask/x/z must be 0' % (TYPE_NAMES[t], r.off))
                k, data, back = self.decode_live(('T', t))
                if data is None:
                    errs.append('%s @%d does not decode' % (TYPE_NAMES[t], r.off))
                elif back:
                    errs.append('%s @%d does not decode; %d versions back does' % (TYPE_NAMES[t], r.off, back))
        pr = self.live(T_PROVENANCE)
        if pr:
            try:
                parse_provenance(self.decode(pr))
            except Exception as ex:
                errs.append('PROVENANCE: %s' % ex)
        cols = self.columns()
        ords = {}
        band_or = 0
        for (x, z), r in cols.items():
            if not (0 <= x < 32768 and 0 <= z < 32768) or ((x << 15) + z) == 0:
                errs.append('COLUMN @%d has an unaddressable key (%d,%d)' % (r.off, x, z))
            if r.mask >> self.bands:
                errs.append('COLUMN (%d,%d) @%d sets band bits >= %d' % (x, z, r.off, self.bands))
            if r.mask == 0 and (r.codec != C_NONE or r.plen):
                errs.append('COLUMN (%d,%d) @%d: empty mask needs codec 0 and no payload' % (x, z, r.off))
            if r.aux in ords:
                errs.append('COLUMN (%d,%d) and %s share ord %d' % (x, z, ords[r.aux], r.aux))
            ords[r.aux] = (x, z)
            band_or |= r.mask
            k, data, back = self.decode_live(('C', x, z))
            if data is None:
                errs.append('COLUMN (%d,%d) @%d does not decode and has no earlier version' % (x, z, r.off))
            elif back:
                errs.append('COLUMN (%d,%d) @%d does not decode; fell back %d version(s)' % (x, z, r.off, back))
            else:
                for i, b in enumerate(b for b in range(self.bands) if r.mask >> b & 1):
                    if data[i * BAND:(i + 1) * BAND].count(0) == BAND:
                        self.warnings.append('COLUMN (%d,%d): band %d stored but all zero (non-canonical)' % (x, z, b))
        sm = self.live(T_SUMMARY)
        if cols and not sm:
            errs.append('no SUMMARY')
        elif sm:
            try:
                lc, no, bo, r1, r2 = struct.unpack(SUMMARY_FMT, self.decode(sm))
                want = (len(cols), (max(ords) + 1) if ords else 0, band_or)
                if (lc, no, bo) != want or r1 or r2:
                    errs.append('SUMMARY (live=%d next_ord=%d band_or=%#x) != live state %s' % (lc, no, bo, want))
            except Exception as ex:
                errs.append('SUMMARY: %s' % ex)
        return self.problems() + errs


def open_emod(path, check_crc=True):
    return Store(path, check_crc)


# ================================================================== export
def export(src, out, force=False, quiet=False):
    st = open_emod(src)
    probs = st.verify()
    if probs and not force:
        die('%s does not verify (use --force to export what decodes):\n  %s' % (src, '\n  '.join(probs)))
    _, hdr, _ = st.decode_live(('T', T_WORLD_HEADER))
    _, creatures, _ = st.decode_live(('T', T_CREATURES))
    trailer = b''
    if st.live(T_SIGN_TRAILER):
        _, trailer, _ = st.decode_live(('T', T_SIGN_TRAILER))
    order = None
    if st.live(T_PROVENANCE):
        _, pv, _ = st.decode_live(('T', T_PROVENANCE))
        order = parse_provenance(pv)['dir_order']
    if hdr is None or creatures is None:
        die('%s: WORLD_HEADER/CREATURES unreadable' % src)
    cols = sorted(st.columns().items(), key=lambda kv: kv[1].aux)
    col = st.bands * BAND
    n = len(cols)
    dir_off = EDEN_HEADER + n * col + len(creatures)
    total = dir_off + n * DIR_ROW + len(trailer)
    hdr = bytearray(hdr)
    hdr[32:40] = struct.pack('<Q', dir_off)
    slot = {k: EDEN_HEADER + i * col for i, (k, _) in enumerate(cols)}
    rows = []
    seen = set()
    for k in (order or []):
        k = tuple(k)
        if k in slot and k not in seen:
            rows.append(k); seen.add(k)
    rows += [k for k, _ in cols if k not in seen]
    tmp = out + '.exporting'
    with open(tmp, 'wb') as f:
        f.write(hdr)
        for k, r in cols:
            rr, data, _ = st.decode_live(('C',) + k)          # --force: newest decodable version, else air
            f.write(join_bands(rr.mask, data, st.bands) if data is not None else bytes(col))
        f.write(creatures)
        f.write(b''.join(struct.pack('<iiQ', x, z, slot[(x, z)]) for x, z in rows))
        f.write(trailer)
        f.flush()
        os.fsync(f.fileno())
    if os.path.getsize(tmp) != total:
        die('export wrote %d B, pre-flight said %d' % (os.path.getsize(tmp), total))
    os.replace(tmp, out)
    st.close()
    if not quiet:
        print('exported %s -> %s: %d columns, %d B (pre-flight %d B)' % (src, out, n, total, total))
    return total


# ================================================================== logical comparison
def logical(path):
    """The engine-visible content of a .eden: header minus directory_offset, creatures, trailer, columns."""
    e = Eden(path)
    h = bytearray(e.header)
    h[32:40] = bytes(8)
    cols = {}
    for c in e.cols:
        cols[(c[0], c[1])] = hashlib.sha256(e.column(c)).digest()
    return dict(header=bytes(h), creatures=e.creatures, trailer=e.trailer, cols=cols), e


def files_equal(a, b):
    if os.path.getsize(a) != os.path.getsize(b):
        return False
    with open(a, 'rb') as fa, open(b, 'rb') as fb:
        while True:
            x, y = fa.read(8 << 20), fb.read(8 << 20)
            if x != y:
                return False
            if not x:
                return True


def roundtrip(src, work, keep=False, determinism=True, epoch=None):
    os.makedirs(work, exist_ok=True)
    stem = os.path.join(work, os.path.basename(src).replace(' ', '_'))
    m1, m2, back = stem + '.emod', stem + '.2.emod', stem + '.export.eden'
    t0 = time.time()
    e = convert(src, m1, epoch=epoch)
    st = open_emod(m1)
    probs = st.verify()
    st.close()
    if probs:
        print('  VERIFY FAILED: ' + '; '.join(probs))
        return False
    export(m1, back)
    ident = files_equal(src, back)
    ok = True
    if e.identical_expected():
        print('  .eden -> .emod -> .eden byte-identical: %s' % ('YES' if ident else 'NO (expected yes)'))
        ok &= ident
    else:
        a, _ = logical(src)
        b, _ = logical(back)
        same = a == b
        print('  byte-identical: %s (not expected: %s); logically identical: %s' % (
            'yes' if ident else 'no', '; '.join(e.notes), 'YES' if same else 'NO'))
        if e.short:
            print('  short spans: ' + ', '.join('(%d,%d)=%d B' % s for s in e.short))
        ok &= same
    if determinism:
        convert(src, m2, epoch=epoch, quiet=True)
        same = files_equal(m1, m2)
        print('  second conversion byte-identical: %s' % ('YES' if same else 'NO'))
        ok &= same
    if not keep:
        for p in (m1, m2, back):
            if os.path.exists(p):
                os.remove(p)
    print('  roundtrip %s (%.1fs)' % ('PASS' if ok else 'FAIL', time.time() - t0))
    return ok


# ================================================================== native vs transposed order
def transpose_plane(p):
    """CC(x,z,y) -> CC(y,z,x) for one 4096-byte plane (the bundled RLE map's order)."""
    out = bytearray(4096)
    for y in range(16):
        for z in range(16):
            s = z * 16 + y
            out[y * 256 + z * 16:y * 256 + z * 16 + 16] = p[s::256][:16]
    return bytes(out)


def measure_order(src, stride=1):
    need_zstd()
    e = Eden(src)
    zc = zstandard.ZstdCompressor(level=3, write_checksum=True, write_content_size=True, write_dict_id=False)
    nat = tr = n = 0
    for i in range(0, len(e.cols), max(1, stride)):
        mask, payload = split_bands(e.column(e.cols[i]), e.bands)
        if not payload:
            continue
        t = b''.join(transpose_plane(payload[o:o + 4096]) for o in range(0, len(payload), 4096))
        nat += len(zc.compress(payload))
        tr += len(zc.compress(t))
        n += 1
    print('%s: %d non-empty columns (stride %d) at zstd-3: native CC(x,z,y) %d B (%.0f B/col), transposed CC(y,z,x) %d B (%.0f B/col): '
          'transposed is %+.1f%% vs native' % (src, n, stride, nat, nat / max(1, n), tr, tr / max(1, n), 100.0 * (tr - nat) / max(1, nat)))
    return nat, tr


# ================================================================== dump
def dump(path, records=False):
    st = open_emod(path)
    print('%s: %d B' % (path, st.size))
    if st.size >= FILE_HEADER:
        print('file header: fmt_version=%d bands=%d created_unix=%d' % (st.fmt_version, st.bands, st.created))
    if records:
        for r in st.recs:
            print('  @%-10d %-12s codec=%-4s mask=%#06x x=%-5d z=%-5d aux=%-7d len=%-7d seq=%d' % (
                r.off, TYPE_NAMES.get(r.type, '?%d' % r.type), CODEC_NAMES.get(r.codec, '?'), r.mask, r.x, r.z, r.aux,
                r.plen, r.seq))
    for s in st.spans:
        print('  damaged/unframeable bytes [%d, %d)' % s)
    print('records: %d framed, %d commits, last seq %d, tail %d B, torn=%s' % (
        len(st.recs), len(st.commits), st.last_seq, st.tail_bytes, st.torn))
    cols = st.columns()
    print('live: %d columns' % len(cols))
    wh = st.live(T_WORLD_HEADER)
    if wh:
        h = st.decode(wh)
        print('world: name=%r version=%d seed=%d' % (h[40:90].split(b'\0')[0].decode('utf8', 'replace'),
                                                     struct.unpack_from('<i', h, 92)[0], struct.unpack_from('<i', h, 0)[0]))
    cr = st.live(T_CREATURES)
    if cr:
        print('creatures: %d slots' % (cr.aux // ENT))
    tr = st.live(T_SIGN_TRAILER)
    print('sign trailer: %d B' % (tr.aux if tr else 0))
    pr = st.live(T_PROVENANCE)
    if pr:
        p = parse_provenance(st.decode(pr))
        print('provenance: %s, %d B, version %d, mtime %d, flags %#x (%s), %d short spans, %d directory rows' % (
            p['name'], p['size'], p['source_version'], p['mtime'], p['flags'],
            ', '.join(v for b, v in PF_NAMES.items() if p['flags'] & b) or 'byte-identical export', len(p['short']),
            len(p['dir_order'])))
    sm = st.live(T_SUMMARY)
    if sm:
        lc, no, bo, _, _ = struct.unpack(SUMMARY_FMT, st.decode(sm))
        print('summary: live_columns=%d next_ord=%d band_or=%#06x' % (lc, no, bo))
    hist = {}
    for r in cols.values():
        hist[r.mask] = hist.get(r.mask, 0) + 1
    print('band masks: ' + ', '.join('%#06x x%d' % kv for kv in sorted(hist.items(), key=lambda kv: -kv[1])[:8]))
    st.close()


# ================================================================== the trailer's sections (Stage D / D.3a)
# The reference for Classes/WorldTrailer.{h,cpp}; the rules are restated in docs/eden-file-format.md
# "The trailer model". Shares no code with the engine on purpose.
TRAILER_TAG = b'\xff\xff\xff\xff'
TRAILER_KINDS = {b'SGN1': 120, b'CMB1': 528}     # record size per known magic


def trailer_parse(t):
    """-> (sections, tail, why). `why` is None when the trailer is understood; otherwise it is opaque and
    a writer must keep it verbatim and refuse edits. A section is (magic, w3, version, [records]) for a
    known magic, or (magic, raw_bytes) for any other (kept verbatim, wrapper included)."""
    if not t:
        return [], b'', None
    if len(t) % DIR_ROW:
        return None, None, 'not a whole number of 16-byte rows'
    for i in range(0, len(t), DIR_ROW):
        if t[i:i + 4] != TRAILER_TAG:
            return None, None, 'row %d is not tagged ff ff ff ff' % (i // DIR_ROW)
    p = b''.join(t[i + 4:i + DIR_ROW] for i in range(0, len(t), DIR_ROW))
    secs, seen, i = [], set(), 0
    while i + 12 <= len(p):
        magic = p[i:i + 4]
        if magic == b'\0\0\0\0':
            break
        inner, w3 = struct.unpack_from('<II', p, i + 4)
        if i + 12 + inner > len(p):
            return None, None, 'section %r at payload byte %d runs past the end' % (magic, i)
        if magic in TRAILER_KINDS:
            rs = TRAILER_KINDS[magic]
            if magic in seen:
                return None, None, 'a second %r section' % magic
            if inner < 12 or p[i + 12:i + 16] != magic:
                return None, None, '%r section without its inner header' % magic
            ver, cnt = struct.unpack_from('<II', p, i + 16)
            if inner - 12 != cnt * rs:
                return None, None, '%r: %d body bytes for %d records of %d' % (magic, inner - 12, cnt, rs)
            b = i + 24
            secs.append((magic, w3, ver, [p[b + k * rs:b + (k + 1) * rs] for k in range(cnt)]))
            seen.add(magic)
        else:
            secs.append((magic, p[i:i + 12 + inner]))
        i += 12 + inner
    tail = p[i:]
    if tail.count(0) != len(tail):
        return None, None, '%d non-zero bytes after the last section' % (len(tail) - tail.count(0))
    return secs, tail, None


def trailer_build(secs, tail, drop_empty=False):
    """Sections + tail -> rows; nothing at all -> b''. `drop_empty` leaves out known sections with no
    records (the engine does that only for a section an edit emptied; untouched ones stay as read)."""
    p = b''
    for s in secs:
        if len(s) == 2:
            p += s[1]
        elif s[3] or not drop_empty:
            body = s[0] + struct.pack('<II', s[2], len(s[3])) + b''.join(s[3])
            p += s[0] + struct.pack('<II', len(body), s[1]) + body
    if not p:
        return b''
    p += tail
    p += bytes((-len(p)) % 12)
    return b''.join(TRAILER_TAG + p[i:i + 12] for i in range(0, len(p), 12))


def trailer_source(path):
    """(trailer bytes, {(x, z): sha256 of the column}) for a .eden or an .emod."""
    with open(path, 'rb') as f:
        is_emod = f.read(8) == MAGIC
    cols = {}
    if is_emod:
        st = open_emod(path)
        if st.errors:
            die('%s: %s' % (path, '; '.join(st.errors)))
        r = st.live(T_SIGN_TRAILER)
        t = st.decode(r) if r else b''
        for k, rc in st.columns().items():
            cols[k] = hashlib.sha256(st.decode(rc)).digest()
        st.close()
    else:
        e = Eden(path)
        t = e.trailer
        for c in e.cols:
            cols[(c[0], c[1])] = hashlib.sha256(e.column(c)).digest()
    return t, cols


def trailer_record_line(magic, r):
    if magic == b'SGN1':
        x, y, z, a, b, c = struct.unpack_from('<6i', r)
        return 'SGN %6d %6d %3d a=%d b=%-2d c=%d %r' % (x, y, z, a, b, c, r[24:].split(b'\0')[0].decode('latin1'))
    if magic == b'CMB1':
        x, y, z, fl = struct.unpack_from('<4i', r)
        return 'CMB %6d %6d %3d flags=%d %r' % (x, y, z, fl, r[16:].split(b'\0')[0].decode('latin1'))
    return '?'


def trailer_cmd(path, check=False, records=False, diff=None):
    t, cols = trailer_source(path)
    secs, tail, why = trailer_parse(t)
    print('%s: trailer %d B (%d rows), %d columns' % (path, len(t), len(t) // DIR_ROW, len(cols)))
    ok = True
    if why:
        print('  OPAQUE: %s (kept verbatim; a writer must refuse edits)' % why)
    else:
        for s in secs:
            if len(s) == 2:
                print('  section %r: unknown, %d B kept verbatim' % (s[0], len(s[1])))
            else:
                print('  section %s: version %d, %d records of %d B' % (s[0].decode(), s[2], len(s[3]), TRAILER_KINDS[s[0]]))
                if records:
                    for r in s[3]:
                        print('    ' + trailer_record_line(s[0], r))
        if tail:
            print('  zero tail: %d B' % len(tail))
        if check:
            same = trailer_build(secs, tail) == t
            print('  check: parse -> serialise %s' % ('byte-identical' if same else 'DIFFERS'))
            ok &= same
    if diff:
        t2, cols2 = trailer_source(diff)
        s2, tail2, why2 = trailer_parse(t2)
        print('diff against %s: trailer %d B' % (diff, len(t2)))
        if why or why2:
            print('  trailers byte-%s (one side opaque)' % ('identical' if t == t2 else 'DIFFERENT'))
        else:
            def flat(ss):
                return [(s[0], k, r) for s in ss if len(s) == 4 for k, r in enumerate(s[3])]
            a, b = flat(secs), flat(s2)
            changed = 0
            for k in range(max(len(a), len(b))):
                ra, rb = (a[k] if k < len(a) else None), (b[k] if k < len(b) else None)
                if ra != rb:
                    changed += 1
                    print('  record %d: %s' % (k, trailer_record_line(ra[0], ra[2]) if ra else '(none)'))
                    print('         -> %s' % (trailer_record_line(rb[0], rb[2]) if rb else '(none)'))
            ua = [s for s in secs if len(s) == 2]
            ub = [s for s in s2 if len(s) == 2]
            print('  %d record(s) differ; unknown sections %s; zero tail %s' % (
                changed, 'same' if ua == ub else 'DIFFER', 'same' if tail == tail2 else '%d -> %d B' % (len(tail), len(tail2))))
        cd = sorted(k for k in set(cols) | set(cols2) if cols.get(k) != cols2.get(k))
        print('  columns: %d vs %d, %d differ%s' % (len(cols), len(cols2), len(cd), (' e.g. %s' % cd[:4]) if cd else ''))
    return ok


def verify_cmd(path, check_crc=True):
    st = open_emod(path, check_crc)
    probs = st.verify()
    for w in st.warnings:
        print('warning: ' + w)
    for p in probs:
        print('ERROR: ' + p)
    if st.tail_bytes:
        print('uncommitted tail: %d B (a torn save; readers ignore it, writers truncate it)' % st.tail_bytes)
    code = 1 if probs else (3 if (st.tail_bytes or st.torn) else 0)
    print('%s: %s (%d columns, last commit seq %d)' % (path, {0: 'OK', 1: 'DAMAGED/INVALID', 3: 'OK + rolled-back tail'}[code],
                                                       len(st.columns()), st.last_seq))
    st.close()
    return code


# ================================================================== append (used by the corrupt gate)
def append_batch(path, columns=None, creatures=None, header=None, codec=C_ZSTD):
    """Append one committed batch: truncate any torn tail, then COLUMN.../CREATURES/WORLD_HEADER, SUMMARY, COMMIT."""
    st = open_emod(path)
    if st.errors:
        die('append: %s' % '; '.join(st.errors))
    cols = {k: r for k, r in st.columns().items()}
    trunc, seq, prev = st.truncate_at, st.last_seq + 1, st.last_commit.off
    bands = st.bands
    next_ord = (max(r.aux for r in cols.values()) + 1) if cols else 0
    masks = {k: r.mask for k, r in cols.items()}
    ords = {k: r.aux for k, r in cols.items()}
    st.close()
    with open(path, 'r+b') as f:
        f.truncate(trunc)
        f.seek(trunc)
        w = Writer(f, seq, prev, codec)
        for (x, z), data in sorted((columns or {}).items()):
            if (x, z) not in ords:
                ords[(x, z)] = next_ord
                next_ord += 1
            mask, payload = split_bands(data, bands)
            masks[(x, z)] = mask
            w.rec(T_COLUMN, payload, mask=mask, x=x, z=z, aux=ords[(x, z)])
        if creatures is not None:
            w.rec(T_CREATURES, creatures)
        if header is not None:
            w.rec(T_WORLD_HEADER, header, compress=False)
        bo = 0
        for m in masks.values():
            bo |= m
        w.rec(T_SUMMARY, summary_payload(len(masks), next_ord, bo), compress=False)
        w.commit()
        f.flush()
        os.fsync(f.fileno())


# ================================================================== synthetic fixtures
def _column(rnd, bands, top, paint_only_band=None):
    c = bytearray(bands * BAND)
    for b in range(top):
        base = b * BAND
        for i in range(0, 4096, 7):
            c[base + i] = rnd.choice((0, 0, 1, 2, 3, 13, 26))
            if rnd.random() < 0.2:
                c[base + 4096 + i] = rnd.randrange(1, 54)
    if paint_only_band is not None:
        base = paint_only_band * BAND
        c[base:base + 4096] = bytes(4096)
        c[base + 4096 + 100] = 7                     # all-air types, non-zero paint: must be stored
    return bytes(c)


def _header(version, name, seed=4242):
    h = bytearray(EDEN_HEADER)
    struct.pack_into('<ifffffff', h, 0, seed, 100.5, 40.0, 200.25, 100.0, 39.0, 200.0, 33.0)
    h[40:40 + len(name)] = name.encode()
    struct.pack_into('<i', h, 92, version)
    h[96:128] = b'0123456789abcdef0123456789abcdef'
    h[132:148] = bytes(range(16))
    struct.pack_into('<i', h, 148, 7)
    h[152:192] = bytes((i * 37) & 0xff for i in range(40))   # 2.1.1-era reserved[] garbage, kept verbatim
    return h


def write_eden(path, version, cols, slots, row_order=None, trailer=b'', creatures=None, layout=None, extra_rows=b'',
               bands=None):
    """cols: [((x,z), data)] in offset order. layout: optional list of offsets (default canonical).
    bands: the stride, when the version does not imply it (a v2 256z world, S.3b)."""
    bands = bands or (16 if version >= 5 else 4)
    col = bands * BAND
    offs = layout or [EDEN_HEADER + i * col for i in range(len(cols))]
    end = max([o + col for o in offs] + [EDEN_HEADER])
    if layout:                                        # a short span: the next record overwrote the tail
        end = max(o for o in offs) + col
    dir_off = end + ENT * slots
    h = _header(version, os.path.basename(path)[:30])
    struct.pack_into('<Q', h, 32, dir_off)
    buf = bytearray(dir_off)
    buf[0:EDEN_HEADER] = h
    for ((x, z), data), o in zip(cols, offs):
        buf[o:o + col] = data
    if creatures is None:
        creatures = b''.join(struct.pack('<7fii3f3f', 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0)[:ENT]
                             for _ in range(slots))
    buf[end:dir_off] = creatures
    order = row_order or list(range(len(cols)))
    rows = b''.join(struct.pack('<iiQ', cols[i][0][0], cols[i][0][1], offs[i]) for i in order)
    with open(path, 'wb') as f:
        f.write(bytes(buf) + rows + extra_rows + trailer)


def sign_trailer(texts):
    payload = b'SGN1' + struct.pack('<II', 1, len(texts))
    for i, t in enumerate(texts):
        payload += struct.pack('<6i', 100 + i, 40, 200, 0, 0, 0) + t.encode().ljust(96, b'\0')
    body = b'SGN1' + struct.pack('<I', len(payload)) + payload
    body += bytes((-len(body)) % 12)
    return b''.join(b'\xff\xff\xff\xff' + body[i:i + 12] for i in range(0, len(body), 12))


def make_fixtures(d):
    """Returns [(path, byte_identical_expected)]. Each covers a case named in the S.1 row."""
    os.makedirs(d, exist_ok=True)
    rnd = random.Random(20261009)
    out = []
    # 64z: paint-only band, an all-air column, creature data, a shuffled (hash-order) directory
    cols = [((4000 + i % 3, 4000 + i // 3), _column(rnd, 4, rnd.randrange(0, 5), 3 if i == 2 else None)) for i in range(9)]
    cols[4] = (cols[4][0], bytes(4 * BAND))
    cr = bytearray(b''.join(struct.pack('<7fii3f3f', 1, 2, 3, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0)[:ENT] for _ in range(200)))
    struct.pack_into('<7fii', cr, 0, 4001.5, 40, 4002.5, 0.1, 0, 0, 90.0, 3, 12)
    p = os.path.join(d, 'f64-basic.eden'); write_eden(p, 4, cols, 200, [5, 1, 7, 0, 8, 3, 6, 2, 4], creatures=bytes(cr)); out.append((p, True))
    # 256z: bands up to 11, a paint-only band, a sign trailer
    cols = [((4100 + i % 4, 3950 + i // 4), _column(rnd, 16, rnd.choice((2, 3, 4, 7, 12)), 5 if i == 1 else None)) for i in range(8)]
    p = os.path.join(d, 'f256-trailer.eden'); write_eden(p, 5, cols, 400, [3, 0, 6, 1, 7, 2, 5, 4],
                                                         trailer=sign_trailer(['HELLO', 'EMOD S.1'])); out.append((p, True))
    # 256z with no creature block (the sibling editor's shape), version 6
    cols = [((10 + i, 20), _column(rnd, 16, 4)) for i in range(4)]
    p = os.path.join(d, 'f256-v6-noslots.eden'); write_eden(p, 6, cols, 0); out.append((p, True))
    # version 2: no creature block either
    cols = [((300, 300 + i), _column(rnd, 4, 3)) for i in range(3)]
    p = os.path.join(d, 'f64-v2.eden'); write_eden(p, 2, cols, 0); out.append((p, True))
    # 256z short span: record 2 starts 24,000 B early (Quarry's 107,072-byte column)
    col = 16 * BAND
    cols = [((50 + i, 60), _column(rnd, 16, 4)) for i in range(4)]
    lay = [EDEN_HEADER, EDEN_HEADER + col, EDEN_HEADER + 2 * col - 24000, EDEN_HEADER + 3 * col - 24000]
    p = os.path.join(d, 'f256-shortspan.eden'); write_eden(p, 5, cols, 400, layout=lay); out.append((p, False))
    # 64z with a dead record, a duplicate key and an interior gate-failing row
    cols = [((70 + i, 80), _column(rnd, 4, 3)) for i in range(4)]
    col = 4 * BAND
    lay = [EDEN_HEADER, EDEN_HEADER + 2 * col, EDEN_HEADER + 3 * col, EDEN_HEADER + 4 * col]   # slot 1 is dead
    p = os.path.join(d, 'f64-dead-dup.eden')
    write_eden(p, 4, cols, 200, [0, 1, 2, 3], layout=lay)
    with open(p, 'r+b') as f:                         # splice an interior bad row and a duplicate of row 0
        f.seek(0, 2)
        sz = f.tell()
        f.seek(sz - 4 * DIR_ROW)
        rows = f.read()
        f.seek(sz - 4 * DIR_ROW)
        f.write(rows[0:16] + struct.pack('<iiQ', -1, 5, 0) + rows[16:] + struct.pack('<iiQ', 70, 80, EDEN_HEADER + col))
    out.append((p, False))
    # S.3b: 256z stamped version 2 (the 2026 game; alpinecraft's shape): 400 slots, content to band 6, a
    # sign trailer. Only the creature gap says 256z -- a `version >= 5` reader sees 64z and drops bands 4-15.
    cols = [((600 + i % 3, 700 + i // 3), _column(rnd, 16, 7)) for i in range(5)]
    p = os.path.join(d, 'f256-v2-signs.eden'); write_eden(p, 2, cols, 400, [2, 0, 4, 1, 3], bands=16,
                                                          trailer=sign_trailer(['V2 AT 256Z'])); out.append((p, True))
    # S.3b: ONE 256z column under version 2 and no creature block: no offset gap to measure, so only the
    # creature-gap test (0 at 131,072) decides
    cols = [((610, 710), _column(rnd, 16, 10))]
    p = os.path.join(d, 'f256-v2-single.eden'); write_eden(p, 2, cols, 0, bands=16); out.append((p, True))
    return out


# ================================================================== the corrupt gate
def corrupt_gate(work, check_crc=True):
    """Flip one byte in every record type (header and payload) and the file header, and truncate mid-record;
    every case must be reported by the reader. With check_crc=False the gate is expected to FAIL."""
    need_zstd()
    os.makedirs(work, exist_ok=True)
    fx = make_fixtures(os.path.join(work, 'fixtures'))
    src = [p for p, _ in fx if p.endswith('f256-trailer.eden')][0]
    base = os.path.join(work, 'gate-base.emod')
    convert(src, base, epoch=0, quiet=True)
    s1 = state_digest(base)
    e = Eden(src)
    c0 = e.column(e.cols[0])
    changed = bytearray(c0); changed[123] ^= 0x5a
    append_batch(base, columns={(e.cols[0][0], e.cols[0][1]): bytes(changed), (5000, 5000): _column(random.Random(9), 16, 2)},
                 creatures=e.creatures[:ENT * 3] + bytes(ENT * 397), header=e.header)
    s2 = state_digest(base)
    st = open_emod(base)
    recs = list(st.recs)
    assert not st.verify() and not st.tail_bytes, 'gate base must verify clean'
    st.close()
    blob = open(base, 'rb').read()
    cases = []
    seen = set()
    for r in recs:                                    # first and last record of every type, header + payload
        for which in ('first', 'last'):
            pick = [q for q in recs if q.type == r.type]
            q = pick[0] if which == 'first' else pick[-1]
            if (q.off, 'h') in seen:
                continue
            seen.add((q.off, 'h'))
            cases.append(('flip %s %s header byte (x field)' % (which, TYPE_NAMES[q.type]), 'flip', q.off + 9))
            if q.plen:
                cases.append(('flip %s %s payload byte' % (which, TYPE_NAMES[q.type]), 'flip', q.poff + q.plen // 2))
    cases.append(('flip file header byte (bands)', 'flip', 10))
    cases.append(('flip file header byte (created_unix)', 'flip', 20))
    last_col = [q for q in recs if q.type == T_COLUMN][-1]
    cases.append(('truncate mid-COLUMN in the last batch', 'trunc', last_col.poff + last_col.plen // 2))
    cases.append(('truncate mid-COMMIT of the last batch', 'trunc', recs[-1].off + 20))
    cases.append(('truncate mid-record in the first batch', 'trunc', recs[3].off + 10))
    undetected = mixed = 0
    for name, kind, at in cases:
        bad = bytearray(blob)
        if kind == 'flip':
            bad[at] ^= 0xff
        else:
            del bad[at:]
        p = os.path.join(work, 'gate-case.emod')
        open(p, 'wb').write(bytes(bad))
        st = open_emod(p, check_crc)
        probs = st.verify() if st.fh_ok else st.problems()
        reported = bool(probs or st.tail_bytes or st.torn)
        sd = None
        if st.fh_ok and st.commits and not st.damage:
            try:
                sd = state_digest_store(st)
            except Exception:
                sd = 'undecodable'
        st.close()
        mix = sd is not None and sd not in (s1, s2) and not probs
        verdict = 'reported' if reported else 'NOT DETECTED'
        if not reported:
            undetected += 1
        if mix:
            mixed += 1
            verdict += ' + MIXED STATE'
        state = '' if sd is None else (' state=commit1' if sd == s1 else ' state=commit2' if sd == s2 else ' state=other')
        print('  %-48s %s%s' % (name, verdict, state))
    os.remove(os.path.join(work, 'gate-case.emod'))
    print('%d cases, %d not detected, %d mixed states (CRC check %s)' % (len(cases), undetected, mixed,
                                                                       'ON' if check_crc else 'DISABLED'))
    if undetected or mixed:
        print('GATE FAILED: corruption went unreported' + (' -- expected, the CRC check is disabled' if not check_crc else ''))
        return False
    print('GATE PASSED: every corrupted or truncated file is reported')
    return True


def state_digest_store(st):
    h = hashlib.sha256()
    for t in (T_WORLD_HEADER, T_CREATURES, T_SIGN_TRAILER):
        r = st.live(t)
        h.update(st.decode(r) if r else b'-')
    for k, r in sorted(st.columns().items()):
        h.update(struct.pack('<iiH', k[0], k[1], r.mask) + st.decode(r))
    return h.hexdigest()


def state_digest(path):
    st = open_emod(path)
    d = state_digest_store(st)
    st.close()
    return d


def stream_matches(src, work, chunks=(1, 7, 4096, 1 << 20), gz=True):
    """S.3 gate 6 (reference half): convert --stream == convert, byte for byte, raw and gzip'd, at
    several feed sizes (1 B exercises every boundary in the state machine)."""
    import gzip
    os.makedirs(work, exist_ok=True)
    ref = os.path.join(work, 'stream-ref.emod')
    convert(src, ref, epoch=0, quiet=True)
    name = os.path.basename(src)
    ok = True
    inputs = [src]
    if gz:
        g = os.path.join(work, name + '.gz')
        with open(src, 'rb') as fi, gzip.open(g, 'wb', compresslevel=6) as fo:
            fo.write(fi.read())
        inputs.append(g)
    big = os.path.getsize(src) > (8 << 20)
    for inp in inputs:
        for ch in chunks:
            if big and ch < 4096:
                continue
            out = os.path.join(work, 'stream-out.emod')
            convert_stream(inp, out, epoch=0, name=name, quiet=True, chunk=ch)
            same = files_equal(ref, out)
            if not same:
                print('  stream (%s, chunk %d) != convert' % (os.path.basename(inp), ch))
                ok = False
            os.remove(out)
    if gz:
        os.remove(inputs[1])
    os.remove(ref)
    print('  convert --stream byte-identical to convert (raw%s, chunks %s): %s' % (
        ' + gz' if gz else '', ','.join(str(c) for c in chunks), 'YES' if ok else 'NO'))
    return ok


def fixtures_pack(d):
    """S.3's checked-in pack (web/tools/fixtures/emod/): every S.1 fixture as a deterministic .eden.gz,
    plus three .emod files only emod.py wrote (two batches; zlib; raw), and expected.txt with the
    SHA-256 of `convert --epoch 0` and the state digest of each. `eden_native --emod-selftest
    --emod-fixtures=DIR` and headless-emod-store-test.js must reproduce every line on every target --
    which is the cross-target byte gate (S.3 gate 4) as well as gates 1 and 6."""
    import gzip, io, shutil, tempfile
    os.makedirs(d, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='emod-pack-')
    lines = ['# written by `emod.py fixtures-pack` (python zstandard %s); regenerate, never hand-edit' % zstandard.__version__,
             '# kind file sha256(convert --epoch 0 output | the .emod, ungzipped) state-digest']

    def gz(path, data):
        buf = io.BytesIO()
        with gzip.GzipFile(filename='', mode='wb', fileobj=buf, mtime=0, compresslevel=9) as g:
            g.write(data)
        open(path, 'wb').write(buf.getvalue())
    for src, _ in make_fixtures(os.path.join(tmp, 'fx')):
        name = os.path.basename(src)
        gz(os.path.join(d, name + '.gz'), open(src, 'rb').read())
        m = os.path.join(tmp, name + '.emod')
        convert(src, m, epoch=0, quiet=True)
        lines.append('convert %s.gz %s %s' % (name, sha256_file(m), state_digest(m)))
        if name == 'f64-basic.eden':
            z = os.path.join(d, 'f64-basic.zlib.emod')
            convert(src, z, codec=C_ZLIB, epoch=0, quiet=True)
            lines.append('read %s %s %s' % (os.path.basename(z), sha256_file(z), state_digest(z)))
        if name == 'f256-trailer.eden':
            n = os.path.join(tmp, 'f256-trailer.none.emod')            # 369 KB raw: shipped gzip'd
            convert(src, n, codec=C_NONE, epoch=0, quiet=True)
            gz(os.path.join(d, 'f256-trailer.none.emod.gz'), open(n, 'rb').read())
            lines.append('read f256-trailer.none.emod.gz %s %s' % (sha256_file(n), state_digest(n)))
            g2 = os.path.join(d, 'f256-trailer.2batch.emod')
            convert(src, g2, epoch=0, quiet=True)
            e = Eden(src)
            c0 = bytearray(e.column(e.cols[0])); c0[123] ^= 0x5a
            append_batch(g2, columns={(e.cols[0][0], e.cols[0][1]): bytes(c0), (5000, 5000): _column(random.Random(9), 16, 2)},
                         creatures=e.creatures[:ENT * 3] + bytes(ENT * 397), header=e.header)
            lines.append('read %s %s %s' % (os.path.basename(g2), sha256_file(g2), state_digest(g2)))
    open(os.path.join(d, 'expected.txt'), 'w').write('\n'.join(lines) + '\n')
    shutil.rmtree(tmp)
    print('\n'.join(lines))


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


# ================================================================== S.6: the bundled default map
def rle_band(rec, off):
    """One RLE band of the bundled map at rec[off:]: (type plane, paint plane, next offset), both in
    the file's transposed CC(y,z,x) order. {int8 type, uint8 color, uint8 count} runs after a
    big-endian u16 length that counts itself (FileManagerHelper.mm fmh_decodeColumnBands)."""
    n = (rec[off] << 8 | rec[off + 1]) - 2
    if n < 0 or off + 2 + n > len(rec):
        die('RLE band length %d runs past the record' % n)
    t = bytearray(); c = bytearray()
    for i in range(off + 2, off + 2 + n, 3):
        cnt = rec[i + 2]
        t += bytes([rec[i]]) * cnt
        c += bytes([rec[i + 1]]) * cnt
    if len(t) != 4096:
        die('RLE band decodes to %d voxels, not 4096' % len(t))
    return bytes(t), bytes(c), off + 2 + n


def untranspose_plane(p):
    """CC(y,z,x) -> CC(x,z,y): out[x*256+z*16+y] = p[y*256+z*16+x]."""
    out = bytearray(4096)
    for x in range(16):
        for z in range(16):
            for y in range(16):
                out[x * 256 + z * 16 + y] = p[y * 256 + z * 16 + x]
    return bytes(out)


def default_map_columns(src):
    """(header, creatures, version, dir_off, size, [(x, z, 4-band CC(x,z,y) column bytes)] in file-offset
    order) of the bundled RLE map."""
    with open(src, 'rb') as f:
        data = f.read()
    h = data[:EDEN_HEADER]
    version, dir_off = check_eden_header(src, h, len(data))
    rows = {}
    for i in range((len(data) - dir_off) // DIR_ROW):
        x, z, o = struct.unpack_from('<iiQ', data, dir_off + i * DIR_ROW)
        if 0 <= x < 32768 and 0 <= z < 32768 and ((x << 15) + z) != 0:
            rows[(x, z)] = o
    slots = 200
    creatures = data[dir_off - ENT * slots:dir_off]
    cols = []
    for (x, z), o in sorted(rows.items(), key=lambda kv: kv[1]):
        t = o
        parts = []
        for _ in range(4):
            tp, cp, t = rle_band(data, t)
            parts.append(untranspose_plane(tp) + untranspose_plane(cp))
        cols.append((x, z, b''.join(parts)))
    return h, creatures, version, dir_off, len(data), cols


def bake_default(src, out, level=19, epoch=None, quiet=False):
    need_zstd()
    t0 = time.time()
    h, creatures, version, dir_off, size, cols = default_map_columns(src)
    created = int(os.stat(src).st_mtime) if epoch is None else epoch
    with open(src, 'rb') as f:
        src_hash = hashlib.sha256(f.read()).digest()
    L = types.SimpleNamespace(
        path=src, header=h, trailer=b'', bands=4, size=size, version=version, flags=PF_NONCANONICAL_LAYOUT,
        dir_rows=len(cols), dead_bytes=0, slots=len(creatures) // ENT, short=[],
        dir_order=[(c[0], c[1]) for c in cols], source_hash=src_hash, cols=[(c[0], c[1]) for c in cols])
    byk = {(c[0], c[1]): c[2] for c in cols}

    def column(ordn, c, w):
        data = byk[(c[0], c[1])]
        mask, payload = split_bands(data, 4)
        return mask, payload, hashlib.sha256(data).digest()

    write_converted(out, L, creatures, created, C_ZSTD, column, level=level)
    if not quiet:
        print('baked %s -> %s: %d columns, zstd-%d, %.1f MB -> %.2f MB (%.0fs)' % (
            src, out, len(cols), level, size / 1048576.0, os.path.getsize(out) / 1048576.0, time.time() - t0))


def verify_default(src, emod_path):
    """Every column of `emod_path` must equal an independent decode of the RLE map: this one expands
    the runs voxel by voxel into the destination index, without the baker's plane helpers."""
    with open(src, 'rb') as f:
        data = f.read()
    dir_off = struct.unpack_from('<Q', data, 32)[0]
    st = open_emod(emod_path)
    live = st.columns()
    bad = n = 0
    for i in range((len(data) - dir_off) // DIR_ROW):
        x, z, o = struct.unpack_from('<iiQ', data, dir_off + i * DIR_ROW)
        if ((x << 15) + z) == 0:
            continue
        r = live.get((x, z))
        if r is None:
            print('  missing column', (x, z)); bad += 1; continue
        got = join_bands(r.mask, st.decode(r), st.bands)
        want = bytearray(4 * BAND)
        p = o
        for band in range(4):
            ln = (data[p] << 8 | data[p + 1]) - 2
            p += 2
            v = 0
            for k in range(p, p + ln, 3):
                for _ in range(data[k + 2]):
                    y, zz, xx = v >> 8, (v >> 4) & 15, v & 15      # transposed CC(y,z,x)
                    d = band * BAND + xx * 256 + zz * 16 + y
                    want[d] = data[k]
                    want[d + 4096] = data[k + 1]
                    v += 1
            p += ln
        n += 1
        if bytes(want) != got:
            bad += 1
            if bad < 5:
                print('  column', (x, z), 'differs')
    extra = len(live) - n
    print('verify-default: %d columns compared, %d differ, %d extra in the .emod -> %s' % (
        n, bad, extra, 'OK' if not bad and not extra else 'FAILED'))
    return not bad and not extra


def selftest(work):
    ok = True
    for p, ident in make_fixtures(os.path.join(work, 'fixtures')):
        print('== ' + os.path.basename(p))
        e = Eden(p)
        if e.identical_expected() != ident:
            print('  fixture prediction wrong: expected identical=%s, tool says %s (%s)' % (ident, e.identical_expected(), e.notes))
            ok = False
        ok &= roundtrip(p, os.path.join(work, 'rt'), epoch=0)
        ok &= stream_matches(p, os.path.join(work, 'rt'))
        for codec in (C_ZLIB, C_NONE):
            m = os.path.join(work, 'rt', 'codec.emod')
            convert(p, m, codec=codec, epoch=0, quiet=True)
            b = os.path.join(work, 'rt', 'codec.eden')
            export(m, b, quiet=True)
            same = logical(p)[0] == logical(b)[0]
            if not same:
                print('  codec %s round trip FAILED' % CODEC_NAMES[codec]); ok = False
            os.remove(m); os.remove(b)
    print('== corrupt gate')
    ok &= corrupt_gate(os.path.join(work, 'gate'))
    print('SELFTEST %s' % ('PASSED' if ok else 'FAILED'))
    return ok


# ================================================================== main
def main():
    argv = ['corrupt-gate' if a == '--corrupt-gate' else a for a in sys.argv[1:]]
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('convert'); c.add_argument('world'); c.add_argument('-o')
    c.add_argument('--codec', choices=('zstd', 'zlib', 'none'), default='zstd'); c.add_argument('--epoch', type=int)
    c.add_argument('--stream', action='store_true'); c.add_argument('--name')
    c.add_argument('--chunk', type=int, default=1 << 20)
    dg = sub.add_parser('digest'); dg.add_argument('world')
    x = sub.add_parser('export'); x.add_argument('world'); x.add_argument('-o'); x.add_argument('--force', action='store_true')
    v = sub.add_parser('verify'); v.add_argument('world'); v.add_argument('--no-crc', action='store_true')
    d = sub.add_parser('dump'); d.add_argument('world'); d.add_argument('--records', action='store_true')
    tr = sub.add_parser('trailer'); tr.add_argument('world'); tr.add_argument('--check', action='store_true')
    tr.add_argument('--records', action='store_true'); tr.add_argument('--diff')
    r = sub.add_parser('roundtrip'); r.add_argument('world'); r.add_argument('--work', default='emod-work')
    r.add_argument('--keep', action='store_true'); r.add_argument('--no-determinism', action='store_true')
    r.add_argument('--epoch', type=int)
    m = sub.add_parser('measure-order'); m.add_argument('world'); m.add_argument('--stride', type=int, default=1)
    f = sub.add_parser('fixtures'); f.add_argument('dir')
    fp = sub.add_parser('fixtures-pack'); fp.add_argument('dir')
    bk = sub.add_parser('bake-default'); bk.add_argument('world'); bk.add_argument('-o')
    bk.add_argument('--level', type=int, default=19); bk.add_argument('--epoch', type=int)
    vd = sub.add_parser('verify-default'); vd.add_argument('eden'); vd.add_argument('emod')
    s = sub.add_parser('selftest'); s.add_argument('--work', default='emod-work')
    g = sub.add_parser('corrupt-gate'); g.add_argument('--no-crc', action='store_true'); g.add_argument('--work', default='emod-work')
    a = ap.parse_args(argv)
    codec = {'zstd': C_ZSTD, 'zlib': C_ZLIB, 'none': C_NONE}
    if a.cmd == 'bake-default':
        bake_default(a.world, a.o or os.path.splitext(a.world)[0] + '.emod', a.level, a.epoch)
    elif a.cmd == 'verify-default':
        sys.exit(0 if verify_default(a.eden, a.emod) else 1)
    elif a.cmd == 'convert':
        if a.stream:
            if a.world == '-' and not a.o:
                die('convert --stream from stdin needs -o')
            stem = a.world[:-3] if a.world.endswith('.gz') else a.world
            convert_stream(a.world, a.o or os.path.splitext(stem)[0] + '.emod', codec[a.codec], a.epoch, a.name,
                           chunk=a.chunk)
        else:
            if a.name:
                die('--name is only for convert --stream')
            convert(a.world, a.o or os.path.splitext(a.world)[0] + '.emod', codec[a.codec], a.epoch)
    elif a.cmd == 'digest':
        print(state_digest(a.world))
    elif a.cmd == 'export':
        export(a.world, a.o or os.path.splitext(a.world)[0] + '.export.eden', a.force)
    elif a.cmd == 'verify':
        sys.exit(verify_cmd(a.world, not a.no_crc))
    elif a.cmd == 'dump':
        dump(a.world, a.records)
    elif a.cmd == 'trailer':
        sys.exit(0 if trailer_cmd(a.world, a.check, a.records, a.diff) else 1)
    elif a.cmd == 'roundtrip':
        sys.exit(0 if roundtrip(a.world, a.work, a.keep, not a.no_determinism, a.epoch) else 1)
    elif a.cmd == 'measure-order':
        measure_order(a.world, a.stride)
    elif a.cmd == 'fixtures':
        for p, ident in make_fixtures(a.dir):
            print('%s (export byte-identical: %s)' % (p, ident))
    elif a.cmd == 'fixtures-pack':
        need_zstd()
        fixtures_pack(a.dir)
    elif a.cmd == 'selftest':
        sys.exit(0 if selftest(a.work) else 1)
    elif a.cmd == 'corrupt-gate':
        sys.exit(0 if corrupt_gate(a.work, not a.no_crc) else 2)


if __name__ == '__main__':
    main()
