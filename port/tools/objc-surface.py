#!/usr/bin/env python3
"""objc-surface.py — the de-ObjC ratchet (G.2b-0, plan D14: WORKING/de-objc-plan-2026-10-24.md).

Counts, per source file, the ObjC message sends, NS/UI/CG/CF/EAGL identifiers and @"" literals
(comments and string bodies stripped). `--check` fails if any file's count ROSE against the checked-in
baseline `port/tools/objc-surface-baseline.txt`, or a file absent from the baseline (a converted or
new one) has any. Every G.2b commit lowers the baseline: `--write` regenerates it.

  python3 -I port/tools/objc-surface.py --check        # CI
  python3 -I port/tools/objc-surface.py --write        # after a commit that lowered it
  python3 -I port/tools/objc-surface.py --report       # print the table, sorted
Scope: Classes/, root Lighting.*, port/src/, native/src/ (.mm .m .cpp .c .h). The tool is a regex
inventory, not a parser: it counts the same way on every run, which is all a ratchet needs."""
import os, re, sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BASELINE = os.path.join(REPO, 'port', 'tools', 'objc-surface-baseline.txt')
EXTS = ('.mm', '.m', '.cpp', '.c', '.h')
ROOTS = ['Classes', 'port/src', 'native/src']
SKIP_DIRS = {'zstd', 'vendor'}          # third-party code; never ObjC

SEND = re.compile(r'\[\s*([A-Za-z_]\w*(?:\[[^\[\]]*\])?(?:\s*(?:->|\.)\s*\w+(?:\[[^\[\]]*\])?)*|\[[^\[\]]*\]|self|super)\s+([a-zA-Z_]\w*)\s*([:\]])')
ID = re.compile(r'\b((?:NS|UI|CG|CF|EAGL)[A-Z]\w*)\b')
NOTOBJC = set('if for while return sizeof case switch else delete new'.split())

def strip(src):
    src = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), src, flags=re.S)
    return re.sub(r'//[^\n]*', '', src)

def strip_strings(src):
    src = re.sub(r'@"(?:\\.|[^"\\])*"', '@S', src)
    src = re.sub(r'"(?:\\.|[^"\\\n])*"', 'S', src)
    return re.sub(r"'(?:\\.|[^'\\\n])'", 'C', src)

def files():
    out = []
    for r in ROOTS:
        for d, dirs, fs in os.walk(os.path.join(REPO, r)):
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
            # not symlinks: native/CMakeLists.txt links wrong-case names (input.h -> Input.h) into Classes/ on Linux
            out += [os.path.join(d, f) for f in fs if f.endswith(EXTS) and not os.path.islink(os.path.join(d, f))]
    out += [os.path.join(REPO, f) for f in ('Lighting.mm', 'Lighting.h') if os.path.exists(os.path.join(REPO, f))]
    return sorted(out)

def measure():
    res = {}
    for f in files():
        s = strip_strings(strip(open(f, encoding='utf-8', errors='replace').read()))
        sends = sum(1 for m in SEND.finditer(s) if m.group(1).split()[0] not in NOTOBJC)
        ids = len(ID.findall(s))
        lits = len(re.findall(r'@S', s))
        if sends or ids or lits:
            res[os.path.relpath(f, REPO)] = (sends, ids, lits)
    return res

def read_baseline():
    base = {}
    for line in open(BASELINE):
        if line.startswith('#') or not line.strip(): continue
        a, b, c, path = line.split(None, 3)
        base[path.strip()] = (int(a), int(b), int(c))
    return base

def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else '--check'
    cur = measure()
    if mode == '--write':
        with open(BASELINE, 'w') as o:
            o.write('# objc-surface baseline (port/tools/objc-surface.py --write). sends ids @"" file. Only ever lower.\n')
            for p, (a, b, c) in sorted(cur.items()):
                o.write(f'{a} {b} {c} {p}\n')
        print(f'objc-surface: wrote {len(cur)} files, {sum(v[0] for v in cur.values())} sends')
    elif mode == '--report':
        for p, v in sorted(cur.items(), key=lambda kv: -kv[1][0]): print(f'{v[0]:5d} {v[1]:5d} {v[2]:5d}  {p}')
        print(f'total: {sum(v[0] for v in cur.values())} sends, {sum(v[1] for v in cur.values())} ids, {sum(v[2] for v in cur.values())} literals')
    elif mode == '--check':
        base = read_baseline()
        bad = 0
        for p, v in sorted(cur.items()):
            b = base.get(p, (0, 0, 0))
            if any(x > y for x, y in zip(v, b)):
                bad += 1
                print(f'objc-surface: RISE {p}: sends/ids/literals {v} > baseline {b}', file=sys.stderr)
        lowered = [p for p in base if tuple(cur.get(p, (0, 0, 0))) < base[p]]
        print(f'objc-surface: {len(cur)} files, {sum(v[0] for v in cur.values())} sends; '
              f'{len(lowered)} below baseline (run --write to lock it in)')
        sys.exit(1 if bad else 0)
    else:
        sys.exit('usage: objc-surface.py --check | --write | --report')

main()
