#!/usr/bin/env python3
"""tutorial.py -- builds the bundled tutorial world, media/Tutorial.emod (UX.10-1).

The world is a normal default-map world (seed 333333, 64z): it stores only the columns the tutorial changes, and
every other column streams from the bundled Eden.emod, exactly as in any world made with New World. Its inputs:
  port/tools/tutorial/layout.py   the stations, as code (site origin, blocks, signs, command blocks)
  port/tools/tutorial/copy.txt    every sign, command-block and hint line (the owner edits this file)
  Eden.emod                       the bundled map the edited columns are decoded from
The output is a pure function of those three files (fixed created_unix, zstd-19 through emod.py's Writer), so CI
rebuilds it and compares bytes (`--check`). emod.py is the one .emod writer; this script never writes records itself.

  python3 port/tools/tutorial.py            rebuild media/Tutorial.emod, port/tools/tutorial/manifest.txt
                                            and Classes/TutorialData.h (the hint copy + station anchors)
  python3 port/tools/tutorial.py --check    rebuild into a temp dir, compare both bytes with the checked-in files,
                                            and prove every refusal fires on a planted bad input
  python3 port/tools/tutorial.py --survey   re-run the site survey that picked layout.py's origin (slow, ~1 min)

Refusals (each stops the build, nothing written): a sign that is empty, longer than 95 characters (the engine keeps
95 + NUL, Classes/WorldTrailer.h) or not printable ASCII; a `say` line with `;`; a script over 511 bytes or not
printable ASCII; more than 512 command blocks; a command block whose cell is not steel (74); a sign whose anchor is
not solid or whose face does not border air; a spawn not standing on solid ground; an edit outside 0 <= y < 64;
a missing copy key; a hint missing for a step or mode, left as `?`, over 80 characters or naming a `{action}` that is
not in Settings.cpp's kKeybinds[]; an output that `emod.py verify` does not pass. Needs `zstandard==0.25.0` (as emod.py does).
"""
import argparse, os, re, struct, sys, tempfile, importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import emod  # noqa: E402

LAYOUT = os.path.join(HERE, 'tutorial', 'layout.py')
COPY = os.path.join(HERE, 'tutorial', 'copy.txt')
MANIFEST = os.path.join(HERE, 'tutorial', 'manifest.txt')
HINTS_H = os.path.join(ROOT, 'Classes', 'TutorialData.h')
SETTINGS_CPP = os.path.join(ROOT, 'port', 'src', 'seam', 'Settings.cpp')
HINT_STEPS = ('look', 'move', 'jump', 'break', 'place', 'picker', 'paint', 'hotbar', 'menu')   # Classes/Tutorial.cpp's order
HINT_MODES = ('kbm', 'touch', 'pad')
HINT_MAX = 80                 # one line on a desktop, two on a phone
OUT = os.path.join(ROOT, 'media', 'Tutorial.emod')
MAP = os.path.join(ROOT, 'Eden.emod')

EPOCH = 1794700800            # 2026-11-15: created_unix, fixed so the file is reproducible
WORLD_NAME = 'Tutorial world'
SEED = 333333                 # DEFAULT_LEVEL_SEED: unsaved columns stream from the bundled map
HEIGHT = 64
SIGN_MAX = 95                 # WorldTrailer.h: text = up to 95 characters + NUL
SCRIPT_MAX = 511              # CmdScript::MAX_LINE, the CMB1 limit
CMD_MAX = 512                 # Emod refuses a 513th command block per world
SIGN_BOARD = 45               # charcoal: what Emod writes for a sign placed with no paint
STEEL = 74
# Not solid for the spawn/sign checks: air, liquids, doors/portals/flowers/golden cube (objects), ladders/vines.
NOT_SOLID = {0, 20, 59, 60, 61, 23, 62, 63, 64, 66, 67, 68, 69, 70, 71, 73, 75, 76, 77, 78, 79, 18, 22}
# The default map's sky palette (FileManager.cpp defaultRegionSkyColors, as a new default world saves it).
SKY_COLORS = bytes([9, 6, 4, 1, 11, 14, 14, 14, 14, 14, 14, 2, 7, 14, 14, 37])
FACES = {'-x': (0, 0, (-1, 0, 0)), '+x': (1, 2, (1, 0, 0)), '-z': (4, 1, (0, 0, -1)), '+z': (5, 3, (0, 0, 1))}


class Refusal(Exception):
    pass


def load_layout():
    spec = importlib.util.spec_from_file_location('tutorial_layout', LAYOUT)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def load_copy(path=COPY):
    out = {}
    with open(path, 'rb') as f:
        for n, raw in enumerate(f, 1):
            line = raw.rstrip(b'\r\n')
            if not line.strip() or line.lstrip().startswith(b'#'):
                continue
            if b':' not in line:
                raise Refusal('%s:%d: not `key: text`' % (path, n))
            k, v = line.split(b':', 1)
            k = k.strip().decode('ascii', 'replace')
            if k in out:
                raise Refusal('%s:%d: key %s given twice' % (path, n, k))
            out[k] = v.strip()
    return out


def keybind_actions():
    """The action ids of Settings.cpp's kKeybinds[] (what `{action}` in a hint may name)."""
    with open(SETTINGS_CPP, encoding='utf-8') as f:
        src = f.read()
    body = src[src.index('static const Keybind kKeybinds[]'):]
    body = body[:body.index('};')]
    return set(re.findall(r'^\s*\{\s*"(\w+)"\s*,\s*"', body, re.M))


def check_hints(copy):
    """hint.<step>.<mode> for every step and mode, written (no `?`), short ASCII, `{action}` naming a real keybind."""
    actions = keybind_actions()
    want = {'hint.%s.%s' % (s, m) for s in HINT_STEPS for m in HINT_MODES}
    for k in sorted(want):
        if k not in copy:
            raise Refusal('copy.txt has no %s' % k)
    for k in sorted(copy):
        if not k.startswith('hint.'):
            continue
        if k not in want:
            raise Refusal('copy.txt has %s, which no hint step uses' % k)
        t = copy[k]
        if t == b'?' or not t:
            raise Refusal('%s is not written yet (text `?`)' % k)
        if not printable(t):
            raise Refusal('%s is not printable ASCII' % k)
        if len(t) > HINT_MAX:
            raise Refusal('%s is %d characters; a hint holds %d' % (k, len(t), HINT_MAX))
        for name in re.findall(rb'\{([^}]*)\}', t):
            if name.decode('ascii') not in actions:
                raise Refusal('%s names {%s}, which is not a keybind action id' % (k, name.decode('ascii')))
        if t.count(b'{') != t.count(b'}') or b'"' in t or b'\\' in t:
            raise Refusal('%s has an unbalanced brace, a quote or a backslash' % k)


def hints_header(copy, signs):
    """Classes/TutorialData.h: the hint copy and the station anchors, so the engine needs no file at run time."""
    out = ['// generated by port/tools/tutorial.py from copy.txt + layout.py: do not edit (UX.10-3).',
           "// The hint copy, keyed `step.mode`, and each sign's anchor cell (x, z, y: the engine's order) for the",
           '// "first within a few blocks of the station" gate. `{action}` is a keybind action id.',
           'struct TutorialHintText { const char* key; const char* text; };',
           'static const TutorialHintText kTutorialHints[] = {']
    for s in HINT_STEPS:
        for m in HINT_MODES:
            out.append('    {"%s.%s", "%s"},' % (s, m, copy['hint.%s.%s' % (s, m)].decode('ascii')))
    out += ['};', 'struct TutorialStation { const char* key; int x, z, y; };', 'static const TutorialStation kTutorialStations[] = {']
    for (x, y, z), _d, key, _r in signs:
        out.append('    {"%s", %d, %d, %d},' % (key[len('sign.'):], x, z, y))
    out.append('};')
    return '\n'.join(out) + '\n'


def printable(b):
    return all(32 <= c < 127 for c in b)


def check_sign_text(key, t):
    if not t:
        raise Refusal('sign %s is empty' % key)
    if not printable(t):
        raise Refusal('sign %s is not printable ASCII' % key)
    if len(t) > SIGN_MAX:
        raise Refusal('sign %s is %d characters; a sign holds %d' % (key, len(t), SIGN_MAX))


def check_script(s):
    if not printable(s):
        raise Refusal('script is not printable ASCII: %r' % s)
    if len(s) > SCRIPT_MAX:
        raise Refusal('script is %d bytes; a command block holds %d: %r...' % (len(s), SCRIPT_MAX, s[:40]))


class World:
    """The bundled map plus edits, in engine coordinates (x, y_up, z)."""

    def __init__(self, map_path):
        self.st = emod.open_emod(map_path)
        if self.st.problems() or self.st.bands != 4:
            raise Refusal('%s is not a clean 64z map' % map_path)
        self.live = self.st.columns()
        self.cols, self.orig = {}, {}

    def col(self, x, z):
        k = (x >> 4, z >> 4)
        c = self.cols.get(k)
        if c is None:
            r = self.live.get(k)
            data = emod.join_bands(r.mask, self.st.decode(r), 4) if r else bytes(4 * emod.BAND)
            self.orig[k] = data
            c = self.cols[k] = bytearray(data)
        return c

    @staticmethod
    def idx(x, z, y):
        return (y >> 4) * emod.BAND + (x & 15) * 256 + (z & 15) * 16 + (y & 15)

    def get(self, x, y, z):
        if not 0 <= y < HEIGHT:
            return 0, 0
        c, i = self.col(x, z), self.idx(x, z, y)
        return c[i], c[i + 4096]

    def put(self, x, y, z, t, paint=0):
        if not 0 <= y < HEIGHT:
            raise Refusal('edit at y=%d is outside the 64-tall world' % y)
        c, i = self.col(x, z), self.idx(x, z, y)
        c[i], c[i + 4096] = t, paint

    def changed(self):
        return sorted(k for k, c in self.cols.items() if bytes(c) != self.orig[k])


class Builder:
    """What layout.build(b) calls. (u, h, v) are relative to the layout's origin and ground; see layout.py."""

    def __init__(self, world, lay, copy):
        self.w, self.L, self.copy = world, lay, copy
        self.signs, self.cmds, self.used = [], [], set()
        self.notes = []

    def at(self, u, h, v):
        return self.L.ORIGIN_X + u, self.L.GROUND_Y + h, self.L.ORIGIN_Z + v

    def text(self, key):
        if key not in self.copy:
            raise Refusal('copy.txt has no %s' % key)
        self.used.add(key)
        return self.copy[key]

    def block(self, u, h, v, t, paint=0):
        self.w.put(*self.at(u, h, v), t, paint)

    def box(self, u0, h0, v0, u1, h1, v1, t, paint=0):
        for u in range(min(u0, u1), max(u0, u1) + 1):
            for v in range(min(v0, v1), max(v0, v1) + 1):
                for h in range(min(h0, h1), max(h0, h1) + 1):
                    self.block(u, h, v, t, paint)

    def ring(self, u0, h, v0, u1, v1, t, paint=0):
        for u in range(u0, u1 + 1):
            for v in range(v0, v1 + 1):
                if u in (u0, u1) or v in (v0, v1):
                    self.block(u, h, v, t, paint)

    def level(self, u0, v0, u1, v1):
        """Grass at h = 0, air above it, dirt filling any dip below it: the surveyed ground, made exact."""
        L = self.L
        for u in range(u0, u1 + 1):
            for v in range(v0, v1 + 1):
                x, g, z = self.at(u, 0, v)
                for y in range(g + 1, HEIGHT):
                    self.w.put(x, y, z, 0)
                self.w.put(x, g, z, L.GRASS, L.GRASS_PAINT)
                y = g - 1
                while y > 0 and self.w.get(x, y, z)[0] in (0, L.LEAVES, L.TREE, L.WATER, 59, 60, 61):
                    self.w.put(x, y, z, L.DIRT, L.DIRT_PAINT)
                    y -= 1

    def clear_trees(self, u0, v0, u1, v1, reach=8):
        """Remove every tree (trunk + leaves, 6-connected) with a cell above the ground inside the box."""
        L = self.L
        tree = (L.LEAVES, L.TREE)
        x0, _, z0 = self.at(u0 - reach, 0, v0 - reach)
        x1, _, z1 = self.at(u1 + reach, 0, v1 + reach)
        ia, _, ka = self.at(u0, 0, v0)
        ib, _, kb = self.at(u1, 0, v1)
        g = L.GROUND_Y
        seen, removed, clipped = set(), 0, 0
        for x in range(ia, ib + 1):
            for z in range(ka, kb + 1):
                for y in range(g + 1, HEIGHT):
                    if (x, y, z) in seen or self.w.get(x, y, z)[0] not in tree:
                        continue
                    stack = [(x, y, z)]
                    seen.add((x, y, z))
                    while stack:
                        p = stack.pop()
                        # a trunk's foot is sunk into the ground: it becomes the ground (grass on top, dirt below)
                        if p[1] > g:
                            self.w.put(*p, 0)
                        else:
                            self.w.put(*p, *((L.GRASS, L.GRASS_PAINT) if p[1] == g else (L.DIRT, L.DIRT_PAINT)))
                        removed += 1
                        for d in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
                            q = (p[0] + d[0], p[1] + d[1], p[2] + d[2])
                            t = self.w.get(*q)[0]
                            if q in seen or t not in tree or (q[1] <= g and t != L.TREE) or q[1] < g - 4:
                                continue
                            if not (x0 <= q[0] <= x1 and z0 <= q[2] <= z1):
                                clipped += 1
                                continue
                            seen.add(q)
                            stack.append(q)
        self.notes.append('trees: %d cells removed%s' % (removed, ', %d clipped at the reach' % clipped if clipped else ''))

    def say(self, key):
        t = self.text(key)
        if b';' in t:
            raise Refusal('%s contains `;`, which would end the say statement' % key)
        if not printable(t):
            raise Refusal('%s is not printable ASCII' % key)
        return t.decode('ascii')

    def sign(self, u, h, v, face, key, board=SIGN_BOARD):
        t = self.text(key)
        check_sign_text(key, t)
        a, c, d = FACES[face]
        x, y, z = self.at(u, h, v)
        self.signs.append(((x, y, z), d, key, struct.pack('<6i', x, z, y, a, board, c) + t.ljust(96, b'\0')))

    def cmd(self, u, h, v, script):
        s = script.encode('ascii') if isinstance(script, str) else script
        check_script(s)
        x, y, z = self.at(u, h, v)
        self.w.put(x, y, z, STEEL, 0)
        self.cmds.append(((x, y, z), struct.pack('<4i', x, z, y, 0) + s.ljust(512, b'\0')))


def validate(b, spawn):
    w = b.w
    if len(b.cmds) > CMD_MAX:
        raise Refusal('%d command blocks; a world holds %d' % (len(b.cmds), CMD_MAX))
    seen = set()
    for p, _ in b.cmds:
        if p in seen:
            raise Refusal('two command blocks at %s' % (p,))
        seen.add(p)
        if w.get(*p)[0] != STEEL:
            raise Refusal('command block at %s sits on type %d, not steel (74)' % (p, w.get(*p)[0]))
    faces = set()
    for p, d, key, _ in b.signs:
        if (p, d) in faces:
            raise Refusal('two signs on one face at %s' % (p,))
        faces.add((p, d))
        if w.get(*p)[0] in NOT_SOLID:
            raise Refusal('sign %s at %s hangs on type %d, not a solid block' % (key, p, w.get(*p)[0]))
        q = (p[0] + d[0], p[1] + d[1], p[2] + d[2])
        if w.get(*q)[0] != 0:
            raise Refusal('sign %s at %s faces type %d, not air' % (key, p, w.get(*q)[0]))
    x, y, z = spawn
    if w.get(x, y - 1, z)[0] in NOT_SOLID or w.get(x, y, z)[0] != 0 or w.get(x, y + 1, z)[0] != 0:
        raise Refusal('spawn %s does not stand on solid ground with two air cells' % (spawn,))


def header(lay, spawn):
    h = bytearray(emod.EDEN_HEADER)
    x, y, z = spawn
    # pos = the player's centre (feet + boxheight/2 = 0.925, as CmdScript's tp writes it), home = the feet cell
    struct.pack_into('<i7f', h, 0, SEED, x + 0.5, y + 0.95, z + 0.5, float(x), float(y), float(z), lay.SPAWN_YAW)
    name = WORLD_NAME.encode('ascii')
    h[40:40 + len(name)] = name
    struct.pack_into('<i', h, 92, 4)
    h[132:148] = SKY_COLORS
    struct.pack_into('<i', h, 148, 10)          # golden cubes, as a new world starts
    return bytes(h)


def build(out, manifest, map_path=MAP, copy_path=COPY, quiet=False, plant=None, hints=None):
    """Build the world into `out` and the manifest into `manifest`. `plant` (tests only) edits the inputs first."""
    lay = load_layout()
    copy = load_copy(copy_path)
    if plant:
        plant(lay, copy)
    w = World(map_path)
    b = Builder(w, lay, copy)
    lay.build(b)
    if plant and getattr(plant, 'after', None):
        plant.after(b)
    spawn = b.at(*lay.SPAWN)
    validate(b, spawn)
    for k in sorted(copy):
        if k.startswith('sign.') and k not in b.used:
            raise Refusal('copy.txt has %s, which no sign uses' % k)
    check_hints(copy)
    trailer = emod.trailer_build([(b'CMB1', 0, 1, [r for _, r in b.cmds]),
                                  (b'SGN1', 0, 1, [r for *_, r in b.signs])], b'')
    # 200 empty EntityData slots (60 B: pos, vel, angle, type = -1, colour, touched, extra2, extra3, extra4)
    creatures = struct.pack('<7fii6f', 0, 0, 0, 0, 0, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0) * 200
    keys = w.changed()
    tmp = out + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(emod.file_header(4, EPOCH, 0))
        wr = emod.Writer(f, 1, 0, emod.C_ZSTD, level=19)
        wr.rec(emod.T_WORLD_HEADER, header(lay, spawn), compress=False)
        wr.rec(emod.T_CREATURES, creatures)
        wr.rec(emod.T_SIGN_TRAILER, trailer)
        band_or = 0
        for ordn, (cx, cz) in enumerate(keys):
            mask, payload = emod.split_bands(bytes(w.cols[(cx, cz)]), 4)
            band_or |= mask
            wr.rec(emod.T_COLUMN, payload, mask=mask, x=cx, z=cz, aux=ordn)
        wr.rec(emod.T_SUMMARY, emod.summary_payload(len(keys), len(keys), band_or), compress=False)
        wr.commit()
    st = emod.open_emod(tmp)
    probs = st.verify()
    tr = st.decode(st.live(emod.T_SIGN_TRAILER))
    st.close()
    secs, _, why = emod.trailer_parse(tr)
    if probs or why:
        os.remove(tmp)
        raise Refusal('the built world does not verify: %s' % '; '.join(probs or [why]))
    os.replace(tmp, out)
    if hints:
        with open(hints, 'w', newline='\n') as f:
            f.write(hints_header(copy, b.signs))
    with open(manifest, 'w', newline='\n') as f:
        f.write('# generated by port/tools/tutorial.py from layout.py + copy.txt; read by eden_native --tutorial-selftest\n')
        f.write('name %s\nsigns %d\ncmds %d\ncolumns %d\nspawn %d %d %d\n' % (WORLD_NAME, len(b.signs), len(b.cmds), len(keys), *spawn))
    if not quiet:
        for n in b.notes:
            print('tutorial: ' + n)
        print('tutorial: %s: %d columns, %d signs, %d command blocks, %d B' % (
            os.path.relpath(out, ROOT), len(keys), len(b.signs), len(b.cmds), os.path.getsize(out)))
    return b


def same(a, b):
    with open(a, 'rb') as f, open(b, 'rb') as g:
        return f.read() == g.read()


def check():
    ok = True
    with tempfile.TemporaryDirectory() as d:
        o, m, hh = os.path.join(d, 'Tutorial.emod'), os.path.join(d, 'manifest.txt'), os.path.join(d, 'TutorialData.h')
        build(o, m, quiet=True, hints=hh)
        for got, want in ((o, OUT), (m, MANIFEST), (hh, HINTS_H)):
            good = os.path.exists(want) and same(got, want)
            print('tutorial --check: %s %s' % (os.path.relpath(want, ROOT), 'matches the rebuild' if good else
                                                'DIFFERS from the rebuild: run port/tools/tutorial.py and commit it'))
            ok &= good

        def plant_sign(lay, copy):
            copy['sign.start'] = b'x' * (SIGN_MAX + 1)

        def plant_nonascii(lay, copy):
            copy['sign.walk'] = b'caf\xc3\xa9'

        def plant_say(lay, copy):
            copy['say.near'] = b'one; two'

        def plant_script(lay, copy):
            copy['say.near'] = b'y' * 600

        def plant_anchor(lay, copy):
            pass
        plant_anchor.after = lambda b: b.w.put(*b.cmds[0][0], 2, 0)      # a command block on stone

        def plant_face(lay, copy):
            pass
        plant_face.after = lambda b: b.w.put(b.signs[0][0][0] - 1, b.signs[0][0][1], b.signs[0][0][2], 2, 0)

        def plant_spawn(lay, copy):
            lay.SPAWN = (lay.SPAWN[0], 5, lay.SPAWN[2])                      # floating

        def plant_missing(lay, copy):
            del copy['sign.finish']

        def plant_qmark(lay, copy):
            copy['hint.jump.pad'] = b'?'

        def plant_action(lay, copy):
            copy['hint.jump.kbm'] = b'{jmup} to jump.'

        def plant_long(lay, copy):
            copy['hint.look.kbm'] = b'x' * (HINT_MAX + 1)

        def plant_nohint(lay, copy):
            del copy['hint.menu.touch']

        for name, plant, needle in (('a 96-character sign', plant_sign, 'characters'),
                                    ('an unwritten hint (`?`)', plant_qmark, 'not written yet'),
                                    ('a hint naming no keybind', plant_action, 'not a keybind action id'),
                                    ('a 81-character hint', plant_long, 'a hint holds'),
                                    ('a missing hint', plant_nohint, 'no hint.menu.touch'),
                                    ('a non-ASCII sign', plant_nonascii, 'printable'),
                                    ('a `;` in a say line', plant_say, '`;`'),
                                    ('a 600-byte script', plant_script, 'bytes'),
                                    ('a command block on stone', plant_anchor, 'not steel'),
                                    ('a sign facing a block', plant_face, 'not air'),
                                    ('a floating spawn', plant_spawn, 'spawn'),
                                    ('a missing copy key', plant_missing, 'no sign.finish')):
            try:
                build(os.path.join(d, 'bad.emod'), os.path.join(d, 'bad.txt'), quiet=True, plant=plant)
                msg, good = 'BUILT (the refusal did not fire)', False
            except Refusal as e:
                msg, good = 'refused: %s' % e, needle in str(e)
            print('tutorial --check: %-26s %s %s' % (name, 'PASS' if good else 'FAIL', msg))
            ok &= good
            if os.path.exists(os.path.join(d, 'bad.emod')):
                ok = False
                print('tutorial --check: a refused build left its output behind: FAIL')
    print('tutorial --check: %s' % ('PASSED' if ok else 'FAILED'))
    return ok


def survey(map_path=MAP, radius=200, length=132, width=20):
    """The site pick recorded in layout.py: of the length x width strips within `radius` of the map centre with no
    water or lava on top, the one minimising (height s.d. + distance / 40). Trees, leaves, vines and flowers do
    not count as the surface (the build clears them)."""
    import statistics
    w = World(map_path)
    c = 4096 * 16
    skip = {0, 5, 6, 22, 73}
    wet = {20, 59, 60, 61, 23, 62, 63, 64}
    top = {}
    for x in range(c - radius, c + radius, 2):
        for z in range(c - radius, c + radius, 2):
            for y in range(HEIGHT - 1, -1, -1):
                t = w.get(x, y, z)[0]
                if t not in skip:
                    top[(x, z)] = (y, t)
                    break
    best = []
    for L, D in ((length, width), (width, length)):
        for x0 in range(c - radius, c + radius - L, 4):
            for z0 in range(c - radius, c + radius - D, 4):
                cells = [top.get((x, z)) for x in range(x0, x0 + L, 2) for z in range(z0, z0 + D, 2)]
                if any(e is None or e[1] in wet for e in cells):
                    continue
                hs = [e[0] for e in cells]
                dist = ((x0 + L / 2 - c) ** 2 + (z0 + D / 2 - c) ** 2) ** .5
                sd = statistics.pstdev(hs)
                best.append((sd + dist / 40, sd, max(hs) - min(hs), dist, x0, z0, L, D, statistics.mean(hs)))
    best.sort()
    print('score   sd    range  dist   x0     z0     LxD     mean height')
    for r in best[:8]:
        print('%.2f  %.2f  %d      %.1f  %d  %d  %dx%d  %.2f' % r)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--survey', action='store_true')
    ap.add_argument('-o', default=OUT)
    a = ap.parse_args()
    emod.need_zstd()
    try:
        if a.survey:
            survey()
        elif a.check:
            sys.exit(0 if check() else 1)
        else:
            build(a.o, MANIFEST if a.o == OUT else os.path.splitext(a.o)[0] + '.manifest.txt',
                  hints=HINTS_H if a.o == OUT else os.path.splitext(a.o)[0] + '.hints.h')
    except Refusal as e:
        print('tutorial.py: refused: %s' % e, file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
