#!/usr/bin/env python3
"""ui-string-gate.py — FT.4's gate: no non-ASCII bytes in engine string literals.

The UI fonts (Jersey/Rubik raster) have no glyph for separators such as U+203A or the middle dot, so a
literal like "Settings \xE2\x80\xBA Storage" draws garbage. This fails (exit 1) on any string literal in
Classes/ and the repo-root sources holding a raw non-ASCII byte or a \\x80-\\xFF / \\u escape. Comments
are skipped. Use ASCII (">" for a path separator). If a literal really must carry Latin-1 text the font
has (accents), put the file:line in ALLOW.

  python3 port/tools/ui-string-gate.py
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXTS = ('.c', '.cc', '.cpp', '.h', '.hpp', '.m', '.mm')
ALLOW = set()          # 'Classes/File.cpp:123'
STR = re.compile(r'"(?:[^"\\\n]|\\.)*"')
BAD = re.compile(r'[^\x00-\x7f]|\\x[89a-fA-F][0-9a-fA-F]|\\u[0-9a-fA-F]{4}|\\U[0-9a-fA-F]{8}|\\[23][0-7]{2}')


def files():
    for d in ('Classes', '.'):
        base = os.path.join(ROOT, d)
        for r, ds, fs in os.walk(base):
            if d == '.' and r != base:
                break
            for f in fs:
                if f.endswith(EXTS):
                    yield os.path.join(r, f)


def main():
    bad = 0
    for p in files():
        rel = os.path.relpath(p, ROOT)
        src = open(p, encoding='utf-8', errors='replace').read()
        src = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), src, flags=re.S)
        for n, line in enumerate(src.split('\n'), 1):
            code = line.split('//')[0] if '"' not in line.split('//')[0] or line.count('"') % 2 == 0 else line
            for s in STR.findall(code):
                if BAD.search(s) and f'{rel}:{n}' not in ALLOW:
                    print(f'{rel}:{n}: non-ASCII in string literal: {s[:80]}')
                    bad += 1
    print('ui-string-gate:', 'FAIL' if bad else 'ok')
    return 1 if bad else 0


sys.exit(main())
