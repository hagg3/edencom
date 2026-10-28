#!/usr/bin/env python3
"""SM.2 spelling pass: #import -> #include, NSLog -> printg, empty EDEN_EXPORT deleted.

Run from the repo root:  python3 port/tools/sm2-spelling.py
Touches only non-comment lines (a line whose first token is // or /* or * is left alone).
Headers that had an #import and no include guard get `#pragma once`. Classes/Lighting.h is
skipped (rule 10: its guard is the decoy). Vendored third-party code is skipped.
"""
import os
import re
import subprocess
import sys

SKIP = re.compile(r'(^|/)(shim/vendor/|vendor/|zstd/)|(^|/)(Lighting\.h)$|(^|/)dr_|(^|/)stb_')
EXTS = ('.h', '.hpp', '.c', '.cpp', '.mm', '.m')
IMPORT = re.compile(r'^(\s*)#import\b')
NSLOG = re.compile(r'\bNSLog\(')
EXPORT_ONLY = re.compile(r'^\s*EDEN_EXPORT\s*$')
EXPORT_TOK = re.compile(r'(^|(?<=[\s;(]))EDEN_EXPORT(?=\s)\s*')


def is_comment(line):
    s = line.lstrip()
    return s.startswith(('//', '/*', '*'))


def guarded(lines):
    head = lines[:40]
    return any(l.lstrip().startswith('#pragma once') or l.lstrip().startswith('#ifndef') for l in head)


def main():
    files = subprocess.run(['git', 'ls-files', 'Classes', 'port/src', 'native'],
                           capture_output=True, text=True, check=True).stdout.split()
    files = [f for f in files if f.endswith(EXTS) and not SKIP.search(f) and os.path.isfile(f)]
    changed = 0
    for path in files:
        with open(path, encoding='utf-8', errors='surrogateescape') as fh:
            src = fh.read().split('\n')
        out = []
        imported = False
        for line in src:
            if not is_comment(line):
                m = IMPORT.match(line)
                if m:
                    line = m.group(1) + '#include' + line[m.end():]
                    imported = True
                if NSLOG.search(line):
                    line = NSLOG.sub('printg(', line)
                if EXPORT_ONLY.match(line):
                    continue
                if 'EDEN_EXPORT' in line:
                    line = EXPORT_TOK.sub('', line)
            out.append(line)
        if imported and path.endswith(('.h', '.hpp')) and not guarded(out):
            out.insert(0, '#pragma once')
        if out != src:
            changed += 1
            with open(path, 'w', encoding='utf-8', errors='surrogateescape') as fh:
                fh.write('\n'.join(out))
    print(f'{changed} files changed', file=sys.stderr)


if __name__ == '__main__':
    main()
