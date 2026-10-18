#!/usr/bin/env python3
"""MP.3a: the --mp-replay fixture tool (WORKING/multiplayer-engine-plan-2026-10-17.md §15).

  mp-replay.py record --edenserver PATH [--out port/tools/fixtures/mp/replay-1.lines]
      Starts a local ewb-server `edenserver` in TESTERS/mp-replay/, has a "builder" client edit near the
      spawn and near a far destination, then records what a second client receives: the join (welcome,
      CAPS, SPAWN, SIGNP burst, SNAPZ reply at the spawn's 128-block lattice point), a second REGION reply at
      the destination's, then live ACTION relays of more builder edits. Writes FILE (the server lines,
      verbatim) and FILE's .expect.
  mp-replay.py expect FILE.lines [--write | --check]
      Recomputes the .expect from the lines alone, with its own decoder (base64, raw inflate, the
      burst merge, ACTION rules) over the flat recipe. --check (CI) exits 1 if the checked-in .expect differs.
      MP.3b's `reconcile.*` keys model leg 3: the spawn burst answered again, then every cell of its
      reconcile box (engine plan §6.3: the lattice point +-208, whole columns) it doesn't name back to
      flat, except the one cell the leg edits after its REGION goes out (`reconcile.keep`); `reconcile2.*`
      is the same reply answering a later REGION, which spares nothing.

  mp-replay.py live --edenserver PATH [--eden native/build/eden_native]
      MP.3b's live join: a local edenserver with builder edits near the spawn, a recorder client's view of
      the spawn's REGION reply decoded here, then `eden_native --mp-join --headless` (connect, CAPS, SPAWN,
      SIGNQ + REGION at the lattice point, Syncing, Loading, Play); its window hash must equal ours.

  mp-replay.py edits --edenserver PATH [--eden ...] [--mutate echo]
      MP.4's two-client leg: a watcher and a builder Emod on a local edenserver, plus a recorder
      connection here. The builder edits through the intent sites; both reconcile; their windows must
      equal our decode of a fresh REGION, each sent ACTION is relayed exactly once, the watcher sends
      none, and the sign round-trips (watcher, relay, eden_signs.txt). --mutate echo must FAIL.

The hash is MpWorld::windowState's: FNV-1a 64 over every cell of the 18x18-column window around a point
that differs from flat, columns in (cx, cz) order then x, z, y inside a column, each fed as
int32 x, int32 y (height), int32 z, uint8 type, uint8 colour, little-endian. Python 3 standard library only.
"""
import argparse, base64, math, os, socket, struct, subprocess, sys, time, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DEFAULT_OUT = os.path.join(HERE, "fixtures", "mp", "replay-1.lines")

SPAWN = (65540.5, 34.0, 65530.5)
DEST = (66548.5, 40.0, 66130.5)
RADIUS = 9            # the session's Render distance cap (engine plan §6.1)
RECONCILE_HALF = 208  # engine plan §6.3: the server's R = 224 less one chunk
KEEP = (int(SPAWN[0]) - 7, 45, int(SPAWN[2]) + 7, 13)   # leg 3's post-REGION local edit: a brick in the air
LIQUID = {20, 23, 59, 60, 61, 62, 63, 64}   # blockinfo IS_LIQUID (Classes/Globals.mm)
NUM_BLOCKS = 127

# ---- the client model ------------------------------------------------------------------------------


def flat_type(y):
    if y == 0: return 1
    if 1 <= y <= 15: return 2
    if 16 <= y <= 31: return 3
    if y == 32: return 8
    return 0


def in_range(x, y, z):
    return 0 <= x < (1 << 19) and 0 <= z < (1 << 19) and 0 <= y < 256


class Overlay:
    """MpWorld's overlay, cell-keyed: (x, y, z) -> (type, colour) for every cell that differs from flat."""

    def __init__(self):
        self.cells = {}
        self.touched = set()
        self.dropped = 0

    def get(self, x, y, z):
        return self.cells.get((x, y, z), (flat_type(y), 0))

    def put(self, x, y, z, t, c):
        if t == flat_type(y) and c == 0: self.cells.pop((x, y, z), None)
        else: self.cells[(x, y, z)] = (t, c)

    def snapz(self, recs):
        for (x, y, z, flag, typ) in recs:
            ok = in_range(x, y, z)
            if ok and flag == 0: ok = 1 <= typ <= NUM_BLOCKS
            elif ok and flag == 3: ok = 0 <= typ <= 54
            elif ok: ok = flag == 1
            if not ok:
                self.dropped += 1
                continue
            key = (x, y, z)
            if key in self.touched: t, c = self.get(x, y, z)
            else:
                self.touched.add(key)
                t, c = flat_type(y), 0
            if flag == 0: t = typ
            elif flag == 1: t, c = 0, 0
            else: c = typ
            self.put(x, y, z, t, c)
        if len(recs) < 3000: self.touched.clear()

    def action(self, who, x, y, z, mode, extra):
        if not in_range(x, y, z):
            self.dropped += 1
            return
        server = who == "server"
        t, c = self.get(x, y, z)
        if mode == 0:
            if extra < 0 or extra > NUM_BLOCKS:
                self.dropped += 1
                return
            if not server and t != 0 and t not in LIQUID: return
            t, c = extra, 0
        elif mode == 1:
            if not server and t == 0: return
            t, c = 0, 0
        elif mode == 3:
            if extra < 0 or extra > 54:
                self.dropped += 1
                return
            if not server and t == 0: return
            c = extra
        else:
            return
        self.put(x, y, z, t, c)


def parse_snapz(line):
    _, count, b64 = line.split(":", 2)
    raw = zlib.decompress(base64.b64decode(b64 + "=" * (-len(b64) % 4)), -15)
    n = int(count)
    if len(raw) != n * 20: raise ValueError("SNAPZ length %d != %d*20" % (len(raw), n))
    return [struct.unpack_from("<5i", raw, i * 20) for i in range(n)]


def replay(lines):
    ov = Overlay()
    spawn = (65536.5, 34.0, 65536.5)
    for line in lines:
        if line.startswith("SNAPZ:"): ov.snapz(parse_snapz(line))
        elif line.startswith("SPAWN:"):
            f = line.split(":")
            spawn = (float(f[1]), float(f[2]), float(f[3]))
        elif line.startswith("ACTION:"):
            f = line.split(":")
            if len(f) >= 7:
                extra = int(f[7]) if len(f) >= 8 else -1
                ov.action(f[1], int(f[3]), int(f[4]), int(f[5]), int(f[6]), extra)
    return ov, spawn


def lattice_point(p):
    """MpWorld's latticeOf: 128 * floor(p / 128 + 0.5)."""
    return 128 * int(math.floor(p / 128.0 + 0.5))


def first_burst(lines):
    """The SNAPZ frames of the first REGION reply: up to and including the first frame of < 3000."""
    out = []
    for line in lines:
        if not line.startswith("SNAPZ:"): continue
        recs = parse_snapz(line)
        out.append(recs)
        if len(recs) < 3000: break
    return out


def reconcile_model(lines, spawn, keep_edit=True):
    """Leg 3: after the whole replay, the spawn burst arrives again and is reconciled at the spawn's point."""
    ov, _ = replay(lines)
    ov.touched = set()
    named = set()
    for recs in first_burst(lines):
        for (x, y, z, flag, typ) in recs: named.add((x, y, z))
        ov.snapz(recs)
    px, pz = lattice_point(spawn[0]), lattice_point(spawn[2])
    c0x, c1x = (px - RECONCILE_HALF) >> 4, (px + RECONCILE_HALF) >> 4
    c0z, c1z = (pz - RECONCILE_HALF) >> 4, (pz + RECONCILE_HALF) >> 4
    keep = KEEP[:3]
    for (x, y, z) in list(ov.cells):
        if c0x <= x >> 4 <= c1x and c0z <= z >> 4 <= c1z and (x, y, z) not in named and (x, y, z) != keep:
            del ov.cells[(x, y, z)]
    if keep_edit: ov.put(KEEP[0], KEEP[1], KEEP[2], KEEP[3], 0)
    return ov


def window_origin(p):
    return int((p + 8.0) // 16) - RADIUS


def window_hash(ov, px, pz):
    ox, oz, side = window_origin(px), window_origin(pz), 2 * RADIUS
    inside = []
    for (x, y, z), (t, c) in ov.cells.items():
        cx, cz = x >> 4, z >> 4
        if ox <= cx < ox + side and oz <= cz < oz + side:
            inside.append((cx, cz, x & 15, z & 15, y, x, z, t, c))
    inside.sort()
    h = 1469598103934665603
    for (_, _, _, _, y, x, z, t, c) in inside:
        for b in struct.pack("<iiiBB", x, y, z, t & 0xFF, c & 0xFF):
            h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % h, len(inside), ox, oz


def read_lines(path):
    with open(path, "rb") as f:
        return [l.rstrip(b"\r").decode("utf-8", "replace") for l in f.read().split(b"\n") if l]


def expect_path(path):
    return (path[:-6] if path.endswith(".lines") else path) + ".expect"


def compute_expect(path, dest):
    ov, spawn = replay(read_lines(path))
    out = ["# mp-replay.py expect: the window state --mp-replay must reproduce (MP.3a/MP.3b). Do not edit by hand."]
    sh, sc, sox, soz = window_hash(ov, spawn[0], spawn[2])
    dh, dc, dox, doz = window_hash(ov, dest[0], dest[2])
    out += ["spawn.x=%g" % spawn[0], "spawn.y=%g" % spawn[1], "spawn.z=%g" % spawn[2],
            "spawn.ox=%d" % sox, "spawn.oz=%d" % soz, "spawn.cells=%d" % sc, "spawn.hash=%s" % sh,
            "dest.x=%g" % dest[0], "dest.y=%g" % dest[1], "dest.z=%g" % dest[2],
            "dest.ox=%d" % dox, "dest.oz=%d" % doz, "dest.cells=%d" % dc, "dest.hash=%s" % dh,
            "overlay.cells=%d" % len(ov.cells), "dropped=%d" % ov.dropped]
    rh, rc, _, _ = window_hash(reconcile_model(read_lines(path), spawn), spawn[0], spawn[2])
    out += ["reconcile.x=%d" % lattice_point(spawn[0]), "reconcile.z=%d" % lattice_point(spawn[2]),
            "reconcile.keep=%d:%d:%d:%d" % KEEP, "reconcile.cells=%d" % rc, "reconcile.hash=%s" % rh]
    # The same reply again, later: the kept cell is older than this REGION, so it is reconciled too.
    r2h, r2c, _, _ = window_hash(reconcile_model(read_lines(path), spawn, keep_edit=False), spawn[0], spawn[2])
    out += ["reconcile2.cells=%d" % r2c, "reconcile2.hash=%s" % r2h]
    return "\n".join(out) + "\n"


def read_expect_dest(path):
    d = {}
    try:
        for l in open(expect_path(path)):
            if "=" in l and not l.startswith("#"):
                k, v = l.strip().split("=", 1)
                d[k] = v
    except OSError:
        return DEST
    return (float(d.get("dest.x", DEST[0])), float(d.get("dest.y", DEST[1])), float(d.get("dest.z", DEST[2])))


# ---- recording against a local edenserver ------------------------------------------------------------


class Conn:
    def __init__(self, port, name):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.s.settimeout(0.2)
        self.buf = b""
        self.lines = []
        self.name = name

    def send(self, line):
        self.s.sendall((line + "\n").encode())

    def pump(self, secs):
        end = time.time() + secs
        while time.time() < end:
            try:
                d = self.s.recv(1 << 16)
                if not d: break
                self.buf += d
            except socket.timeout:
                pass
            while b"\n" in self.buf:
                l, self.buf = self.buf.split(b"\n", 1)
                self.lines.append(l.decode("utf-8", "replace"))

    def wait_for(self, pred, secs=10):
        end = time.time() + secs
        while time.time() < end:
            if any(pred(l) for l in self.lines): return True
            self.pump(0.2)
        return False


def lattice(p):
    return 128 * int(round(p / 128.0))


def builder_edits(cx, cz, tag):
    """(x, y, z, mode, extra) around (cx, cz): builds, painted builds, painted naturals, mined naturals,
    a high cell, chunk-border cells, a painted floor; a few are outside the 18-column window on purpose."""
    x0, z0 = int(cx), int(cz)
    e = []
    for y in range(33, 41): e.append((x0, y, z0, 0, 13))                     # a brick tower
    e += [(x0 + 5, 33, z0 - 5, 0, 14), (x0 + 5, 33, z0 - 5, 3, 12)]           # painted cobblestone
    e += [(x0 - 10, 32, z0 - 10, 3, 30)]                                     # painted natural grass (flag 3 only)
    e += [(x0 + 10, 32, z0 + 10, 1, -1), (x0 + 11, 5, z0 + 11, 1, -1)]       # mined grass, mined stone
    e += [(x0 + 60, 250, z0 - 50, 0, 16)]                                    # a high crystal (256z)
    bx = (x0 // 16) * 16
    e += [(bx - 1, 33, z0, 0, 7), (bx, 33, z0, 0, 7), (bx, 34, z0, 3, 44)]   # a chunk border; paint on a build
    for i in range(10):
        for j in range(10):
            e.append((x0 - 120 + i, 33, z0 + 70 + j, 0, 7))
            if (i + j) % 2 == 0: e.append((x0 - 120 + i, 33, z0 + 70 + j, 3, (i * 10 + j) % 54 + 1))
    e += [(x0 - 145, 33, z0, 0, 13), (x0 + 145, 33, z0, 0, 13)]              # the window's -x edge, and just past +x
    e += [(x0 + 3, 20, z0 + 3, 0, 9)]                                        # TNT buried in dirt (never lit)
    if tag == "spawn":
        # A painted 60x60 wall: 7,200 records, so the reply spans three frames and some cell's flag 0 and
        # flag 3 records straddle a frame boundary (the burst merge must carry them across).
        for i in range(60):
            for y in range(41, 101):
                e += [(x0 - 40 + i, y, z0 + 20, 0, 14), (x0 - 40 + i, y, z0 + 20, 3, (i + y) % 54 + 1)]
    return e


def record(args):
    srv = os.path.abspath(args.edenserver)
    work = os.path.join(REPO, "TESTERS", "mp-replay")
    if os.path.isdir(work):
        for f in os.listdir(work): os.remove(os.path.join(work, f))
    os.makedirs(work, exist_ok=True)
    port = args.port
    log = open(os.path.join(work, "server.log"), "w")
    p = subprocess.Popen([srv, "--port", str(port), "--world", "mp.model", "--action-rate", "0",
                          "--spawn", "%.2f:%.2f:%.2f" % SPAWN, "--no-control-socket"], cwd=work, stdout=log, stderr=log)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.1)
        b = Conn(port, "builder")
        b.send("JOIN:builder:0::zr")
        if not b.wait_for(lambda l: l.startswith("[Server] Welcome")): raise SystemExit("builder: no welcome")
        for (x, y, z, m, ex) in builder_edits(SPAWN[0], SPAWN[2], "spawn") + builder_edits(DEST[0], DEST[2], "dest"):
            b.send("ACTION:%d:%d:%d:%d" % (x, y, z, m) + (":%d" % ex if m in (0, 3) else ""))
        b.send("SIGNP:%d:40:%d:3:2:0:Hello from MP.3a" % (int(SPAWN[0]), int(SPAWN[2])))
        b.pump(1.0)

        r = Conn(port, "recorder")
        r.send("JOIN:recorder:0::zr")
        r.send("SIGNQ")
        r.send("REGION:%d:%d" % (lattice(SPAWN[0]), lattice(SPAWN[2])))
        def burst_done(start):
            return lambda _l: any(l.startswith("SNAPZ:") and int(l.split(":")[1]) < 3000 for l in r.lines[start:])
        if not r.wait_for(burst_done(0)): raise SystemExit("recorder: no spawn SNAPZ reply")
        r.pump(1.0)   # the server drops a REGION inside its 750 ms gap (protocol "Pacing")
        n0 = len(r.lines)
        r.send("REGION:%d:%d" % (lattice(DEST[0]), lattice(DEST[2])))
        if not r.wait_for(burst_done(n0)): raise SystemExit("recorder: no destination SNAPZ reply")
        # Live relays: a peer build into air, one onto a block (the client keeps its block), a paint,
        # a mine of natural grass, a burn (the client ignores it; the next REGION brings the result).
        x0, z0 = int(SPAWN[0]), int(SPAWN[2])
        for line in ["ACTION:%d:33:%d:0:13" % (x0 + 1, z0 + 1), "ACTION:%d:34:%d:0:2" % (x0, z0),
                     "ACTION:%d:33:%d:3:5" % (x0 + 1, z0 + 1), "ACTION:%d:32:%d:1" % (x0 + 2, z0 + 2),
                     "ACTION:%d:32:%d:2" % (x0 + 3, z0 + 3)]:
            b.send(line)
        b.pump(0.5)
        r.pump(1.5)
        b.s.close()
        r.s.close()
    finally:
        p.terminate()
        p.wait()
        log.close()
    lines = [l for l in r.lines if l != "PONG"]
    out = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", newline="\n") as f:
        for l in lines: f.write(l + "\n")
    with open(expect_path(out), "w", newline="\n") as f:
        f.write(compute_expect(out, DEST))
    acts = sum(1 for l in lines if l.startswith("ACTION:"))
    snaps = sum(1 for l in lines if l.startswith("SNAPZ:"))
    print("recorded %d lines (%d SNAPZ frames, %d ACTION relays) -> %s" % (len(lines), snaps, acts, out))
    print(open(expect_path(out)).read(), end="")


def start_server(srv, work, port):
    if os.path.isdir(work):
        for f in os.listdir(work):
            fp = os.path.join(work, f)
            if os.path.isfile(fp): os.remove(fp)
    os.makedirs(work, exist_ok=True)
    log = open(os.path.join(work, "server.log"), "w")
    p = subprocess.Popen([srv, "--port", str(port), "--world", "mp.model", "--action-rate", "0",
                          "--spawn", "%.2f:%.2f:%.2f" % SPAWN, "--no-control-socket"], cwd=work, stdout=log, stderr=log)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    return p, log


def live(args):
    srv = os.path.abspath(args.edenserver)
    work = os.path.join(REPO, "TESTERS", "mp-join-live")
    port = args.port
    p, log = start_server(srv, work, port)
    try:
        b = Conn(port, "builder")
        b.send("JOIN:builder:0::zr")
        if not b.wait_for(lambda l: l.startswith("[Server] Welcome")): raise SystemExit("builder: no welcome")
        for (x, y, z, m, ex) in builder_edits(SPAWN[0], SPAWN[2], "spawn"):
            b.send("ACTION:%d:%d:%d:%d" % (x, y, z, m) + (":%d" % ex if m in (0, 3) else ""))
        b.pump(1.0)
        r = Conn(port, "recorder")
        r.send("JOIN:recorder:0::zr")
        r.send("REGION:%d:%d" % (lattice_point(SPAWN[0]), lattice_point(SPAWN[2])))
        if not r.wait_for(lambda _l: any(l.startswith("SNAPZ:") and int(l.split(":")[1]) < 3000 for l in r.lines)):
            raise SystemExit("recorder: no SNAPZ reply")
        r.s.close()
        ov = Overlay()
        for recs in first_burst(r.lines): ov.snapz(recs)
        want, cells, ox, oz = window_hash(ov, SPAWN[0], SPAWN[2])
        docs = os.path.join(work, "docs")
        os.makedirs(docs, exist_ok=True)
        open(os.path.join(docs, ".migrated-from-documents"), "w").write("1")
        cmd = [os.path.abspath(args.eden), "--headless", "--mp-join=127.0.0.1:%d" % port, "--mp-name=EmodGate",
               "--docs=" + docs, "--frames=60"]
        out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120).stdout.decode("utf-8", "replace")
        b.s.close()
    finally:
        p.terminate()
        p.wait()
        log.close()
    open(os.path.join(work, "eden.log"), "w").write(out)
    got = None
    for l in out.splitlines():
        if l.startswith("[mp] mp.join") or l.startswith("[eden-mp-join]"): print(l)
        if l.startswith("[eden-mp-join] window hash="): got = l.split("hash=")[1].split()[0]
    ok = "[eden-mp-join] ALL PASS" in out and got == want
    print("mp-replay.py live: Emod window %s, ours %s (%d cells, window %d,%d) -> %s" % (got, want, cells, ox, oz,
                                                                                         "PASS" if ok else "FAIL"))
    return 0 if ok else 1


def edits(args):
    """MP.4's two-client leg (engine plan §15 "--mp-selftest"): a watcher Emod joins, a builder Emod joins
    and edits through the intent sites, a recorder connection here sees every relay. Then both Emods
    re-ask their REGION and reconcile. Pass: both windows equal our decode of a fresh REGION reply (the
    server's truth); every ACTION the builder sent was relayed exactly once and the watcher sent none
    (--mutate echo must fail this); the builder's sign reached the watcher and eden_signs.txt unchanged."""
    import threading
    srv = os.path.abspath(args.edenserver)
    eden = os.path.abspath(args.eden)
    work = os.path.join(REPO, "TESTERS", "mp-edits-live")
    port = args.port
    p, log = start_server(srv, work, port)
    go = os.path.join(work, "go")
    def docs(name):
        d = os.path.join(work, "docs-" + name)
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, ".migrated-from-documents"), "w").write("1")
        return d
    out = {"watcher": [], "builder": ""}
    ok = False
    try:
        rec = Conn(port, "recorder")
        rec.send("JOIN:recorder:0::zr")
        if not rec.wait_for(lambda l: l.startswith("[Server] Welcome")): raise SystemExit("recorder: no welcome")
        stop = threading.Event()
        def keep_reading():   # the recorder reads all along, so the server never backs up on it
            while not stop.is_set(): rec.pump(0.2)
        rth = threading.Thread(target=keep_reading, daemon=True)
        rth.start()
        wcmd = [eden, "--headless", "--mp-join=127.0.0.1:%d" % port, "--mp-name=EmodB", "--mp-hold=" + go,
                "--docs=" + docs("watcher"), "--frames=30"] + (["--mp-mutate=" + args.mutate] if args.mutate else [])
        wp = subprocess.Popen(wcmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        playable = threading.Event()
        def reader():
            for raw in wp.stdout:
                l = raw.decode("utf-8", "replace").rstrip("\n")
                out["watcher"].append(l)
                if l.startswith("[eden-mp-join] window hash="): playable.set()
        th = threading.Thread(target=reader, daemon=True)
        th.start()
        if not playable.wait(60): raise SystemExit("watcher: never playable")
        bcmd = [eden, "--headless", "--mp-join=127.0.0.1:%d" % port, "--mp-name=EmodA", "--mp-edits",
                "--docs=" + docs("builder"), "--frames=30"]
        out["builder"] = subprocess.run(bcmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        timeout=180).stdout.decode("utf-8", "replace")
        open(go, "w").write("1")
        wp.wait(timeout=180)
        th.join(5)
        rec.pump(0.5)
        stop.set()
        rth.join(5)
        n0 = len(rec.lines)
        rec.send("REGION:%d:%d" % (lattice_point(SPAWN[0]), lattice_point(SPAWN[2])))
        if not rec.wait_for(lambda _l: any(l.startswith("SNAPZ:") and int(l.split(":")[1]) < 3000 for l in rec.lines[n0:])):
            raise SystemExit("recorder: no SNAPZ reply")
        rec.s.close()
        ok = True
    finally:
        p.terminate()
        p.wait()
        log.close()
    wout = "\n".join(out["watcher"])
    bout = out["builder"]
    open(os.path.join(work, "watcher.log"), "w").write(wout)
    open(os.path.join(work, "builder.log"), "w").write(bout)
    ov = Overlay()
    for recs in first_burst(rec.lines[n0:]): ov.snapz(recs)
    want, cells, _, _ = window_hash(ov, SPAWN[0], SPAWN[2])

    def field(text, role, key):
        for l in text.splitlines():
            if l.startswith("[eden-mp-join] edits role=" + role):
                return l.split(key + "=")[1].split()[0]
        return None
    def signs(text):
        return [l.split("sign ", 1)[1] for l in text.splitlines() if l.startswith("[eden-mp-join] sign ")]
    bh, wh = field(bout, "builder", "hash"), field(wout, "watcher", "hash")
    wpre = (field(wout, "watcher", "pre") or "/").split("/")[0]   # before its closing reconcile: the relays alone
    sent = int(field(bout, "builder", "actions") or -1)
    relA = sum(1 for l in rec.lines if l.startswith("ACTION:EmodA:"))
    relB = sum(1 for l in rec.lines if l.startswith("ACTION:EmodB:"))
    bsigns, wsigns = signs(bout), signs(wout)
    stored = []
    try:
        stored = [l.rstrip("\n") for l in open(os.path.join(work, "eden_signs.txt")) if l.strip() and not l.startswith("#")]
    except OSError:
        pass
    relayedSigns = [l[len("SIGNP:server:"):] for l in rec.lines if l.startswith("SIGNP:server:")]
    checks = [
        ("both Emods ran clean", ok and "[eden-mp-join] ALL PASS" in bout and "[eden-mp-join] ALL PASS" in wout),
        ("builder %s == watcher %s == server %s (%d cells)" % (bh, wh, want, cells), bh == want and wh == want),
        ("the watcher's window from the relays alone (before reconcile) %s == server" % wpre, wpre == want),
        ("every ACTION the builder sent relayed once: sent %d, relayed %d" % (sent, relA), sent > 0 and relA == sent),
        ("the watcher sent no ACTION (no echo): %d" % relB, relB == 0),
        ("the sign: builder %s, watcher %s, relayed %s, eden_signs.txt %s" % (bsigns, wsigns, relayedSigns, stored),
         len(bsigns) == 1 and bsigns == wsigns and bsigns == relayedSigns and bsigns == stored),
    ]
    for name, good in checks: print("mp-replay.py edits: %-4s %s" % ("PASS" if good else "FAIL", name))
    allok = all(g for _, g in checks)
    print("mp-replay.py edits: %s%s" % ("ALL PASS" if allok else "FAILURES", " [mutation %s]" % args.mutate if args.mutate else ""))
    return 0 if allok else 1


def expect(args):
    text = compute_expect(args.file, read_expect_dest(args.file))
    path = expect_path(args.file)
    if args.write:
        with open(path, "w", newline="\n") as f: f.write(text)
        print("wrote " + path)
    elif args.check:
        try: have = open(path).read().replace("\r\n", "\n")
        except OSError: have = ""
        if have != text:
            sys.stdout.write("mp-replay.py: %s is stale; expected:\n%s" % (path, text))
            return 1
        print("mp-replay.py: %s matches the replay" % path)
    else:
        sys.stdout.write(text)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record")
    r.add_argument("--edenserver", required=True)
    r.add_argument("--out", default=DEFAULT_OUT)
    r.add_argument("--port", type=int, default=27941)
    lv = sub.add_parser("live")
    lv.add_argument("--edenserver", required=True)
    lv.add_argument("--eden", default=os.path.join(REPO, "native", "build", "eden_native"))
    lv.add_argument("--port", type=int, default=27942)
    ed = sub.add_parser("edits")
    ed.add_argument("--edenserver", required=True)
    ed.add_argument("--eden", default=os.path.join(REPO, "native", "build", "eden_native"))
    ed.add_argument("--port", type=int, default=27943)
    ed.add_argument("--mutate", default="")
    e = sub.add_parser("expect")
    e.add_argument("file")
    e.add_argument("--write", action="store_true")
    e.add_argument("--check", action="store_true")
    a = ap.parse_args()
    if a.cmd == "record": return record(a) or 0
    if a.cmd == "live": return live(a)
    if a.cmd == "edits": return edits(a)
    return expect(a)


if __name__ == "__main__":
    sys.exit(main())
