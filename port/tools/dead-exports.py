#!/usr/bin/env python3 -I
"""QA.21 gate: no dead `eden_*` exports. Relinks the Mac build with -Wl,-map, takes the dead-stripped
`_eden_*` symbols, and fails if one has no call site in Classes/ or port/src/ (a line that is only a
declaration/definition does not count). -O3 also strips out-of-line copies of inlined live functions, which
is why the source grep is the judge, not the map. Names used only by iOS/Windows/Linux arms are in
dead-exports-allow.txt (one per line, with a reason after #).

usage: dead-exports.py [build-dir]   (default native/build; run from anywhere; macOS only)
"""
import os, re, subprocess, sys
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
bd = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(root, 'native', 'build')
link = open(os.path.join(bd, 'CMakeFiles/eden_native.dir/link.txt')).read().strip()
cmd = link.replace('-o eden_native', '-Wl,-map,map-dead.txt -o eden_native_map')
subprocess.run(cmd, shell=True, cwd=bd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
mp = open(os.path.join(bd, 'map-dead.txt'), errors='replace').read()
for f in ('map-dead.txt', 'eden_native_map'):
    try: os.remove(os.path.join(bd, f))
    except OSError: pass
dead = sorted(set(re.findall(r'_(eden_[a-z0-9_]+)', mp.split('# Dead Stripped Symbols', 1)[1])))
allow = set()
ap = os.path.join(root, 'port/tools/dead-exports-allow.txt')
if os.path.exists(ap):
    allow = {l.split('#')[0].strip() for l in open(ap) if l.split('#')[0].strip()}
src = []
for d in ('Classes', 'port/src', 'port/tools'):
    for dp, _, fs in os.walk(os.path.join(root, d)):
        if 'vendor' in dp: continue
        for f in fs:
            if f.endswith(('.cpp', '.c', '.h', '.mm', '.hpp')):
                src.append(os.path.join(dp, f))
lines = []
for p in src:
    for l in open(p, errors='replace'):
        if 'eden_' in l: lines.append(l)
declre = re.compile(r'^\s*(extern\s+("C"\s+)?)?(static\s+)?(const\s+)?\w[\w:\*&\s]*?[\s\*&]NAME\s*\([^;{]*\)\s*(\{.*|;.*)?$')
bad = []
for n in dead:
    if n in allow: continue
    r = declre.pattern.replace('NAME', n)
    r = re.compile(r)
    use = 0
    for l in lines:
        if not re.search(r'\b' + n + r'\b', l): continue
        code = l.split('//')[0]
        if n not in code: continue
        m = r.match(code)
        if m and not re.search(r'\b(return|else|case)\b', code.split(n)[0]) and '=' not in code.split(n)[0]: continue
        use += 1
    if not use: bad.append(n)
if bad:
    print('dead eden_* exports (no call site):'); [print('  ' + n) for n in bad]; sys.exit(1)
print('dead-exports: ok (%d stripped by the linker, all with call sites or allowed)' % len(dead))
