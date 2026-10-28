#!/usr/bin/env python3
"""gl-call-gate.py — G.5b-7's grep gate: no GL call outside the renderer.

Since G.5b-7 (2026-10-28) the engine (Classes/, the repo-root sources) and the host (port/src/) reach GL
only through R:: (Classes/Renderer.h); the GL backend is port/src/render/. This fails (exit 1) on any
LIVE `gl[A-Z]...(` call anywhere else: comments, string literals and `#if 0` blocks are skipped, so the
commented-out archaeology the engine keeps (root CLAUDE.md rule 9) does not count. The engine's
<OpenGLES/ES1/gl.h> declares no functions any more, so most such calls fail to compile anyway; this
catches the rest (a file that includes real GL, or a platform header that exports legacy GL names, as
macOS's OpenGL.framework does).

  python3 port/tools/gl-call-gate.py            # the gate (CI, linux job)
  python3 port/tools/gl-call-gate.py --tokens   # also list live GL_* tokens outside the renderer (info)
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SCAN = ['Classes', 'port/src', '.']            # '.' = the repo-root sources only (not recursive)
ALLOWED = ['port/src/render/', 'port/src/shim/vendor/']
EXTS = ('.c', '.cc', '.cpp', '.h', '.hpp', '.m', '.mm')
CALL = re.compile(r'\bgl[A-Z][A-Za-z0-9_]*\s*\(')
TOKEN = re.compile(r'\bGL_[A-Z0-9_]+\b')


def strip(src):
    """Blank comments and string/char literals, keeping line numbers."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * src.count('\n', i, j))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and src[j] != c and src[j] != '\n':
                j += 2 if src[j] == '\\' else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def live_lines(path):
    lines = strip(open(path, errors='replace').read()).split('\n')
    depth = 0
    for no, ln in enumerate(lines, 1):
        t = ln.strip()
        if depth:
            if re.match(r'#\s*if', t):
                depth += 1
            elif re.match(r'#\s*endif', t):
                depth -= 1
            elif depth == 1 and re.match(r'#\s*(else|elif)\b', t):
                depth = 0
            continue
        if re.match(r'#\s*if\s+0\b', t):
            depth = 1
            continue
        yield no, ln


def files():
    for top in SCAN:
        base = os.path.join(ROOT, top)
        if top == '.':
            for f in sorted(os.listdir(base)):
                if f.endswith(EXTS) and os.path.isfile(os.path.join(base, f)):
                    yield f
            continue
        for d, _, fs in os.walk(base):
            for f in sorted(fs):
                if f.endswith(EXTS):
                    yield os.path.relpath(os.path.join(d, f), ROOT)


def main():
    tokens = '--tokens' in sys.argv[1:]
    bad = 0
    for rel in files():
        rel = rel.replace(os.sep, '/')
        if any(rel.startswith(a) for a in ALLOWED):
            continue
        for no, ln in live_lines(os.path.join(ROOT, rel)):
            for m in CALL.finditer(ln):
                bad += 1
                print(f'{rel}:{no}: {m.group(0)[:-1].strip()}   | {ln.strip()[:100]}')
            if tokens:
                for m in TOKEN.finditer(ln):
                    print(f'{rel}:{no}: token {m.group(0)}')
    if bad:
        print(f'gl-call-gate: FAIL — {bad} live GL call(s) outside port/src/render/ '
              f'(use R::, Classes/Renderer.h)', file=sys.stderr)
        return 1
    print('gl-call-gate: OK — no live GL call outside port/src/render/', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
