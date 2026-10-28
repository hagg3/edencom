#!/usr/bin/env python3 -I
"""SM.6: BOOL/YES/NO/TRUE/FALSE/nil -> bool/true/false/nullptr in code (not comments, strings, #if 0).
Usage: bool-modernise.py FILE... (rewrites in place). Skips eden_base.h and Objective-C++ walls by caller."""
import re, sys
MAP = {'BOOL': 'bool', 'YES': 'true', 'NO': 'false', 'TRUE': 'true', 'FALSE': 'false', 'nil': 'nullptr'}
WORD = re.compile(r'\b(BOOL|YES|NO|TRUE|FALSE|nil)\b')

def convert(src):
    out = []; i = 0; n = len(src); changed = 0
    while i < n:
        c = src[i]
        if (i == 0 or src[i - 1] == '\n') and re.compile(r'[ \t]*#[ \t]*if[ \t]+0\b').match(src, i):
            depth = 0; j = i
            while j < n:
                e = src.find('\n', j); e = n if e < 0 else e
                ln = src[j:e]
                if re.match(r'[ \t]*#[ \t]*if', ln): depth += 1
                elif re.match(r'[ \t]*#[ \t]*endif', ln): depth -= 1
                j = min(e + 1, n)
                if depth == 0: break
            out.append(src[i:j]); i = j
        elif src.startswith('//', i):
            j = src.find('\n', i); j = n if j < 0 else j
            out.append(src[i:j]); i = j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2); j = n if j < 0 else j + 2
            out.append(src[i:j]); i = j
        elif c in '"\'':
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == '\\' else 1
            out.append(src[i:j + 1]); i = j + 1
        elif c.isalpha() or c == '_':
            m = re.compile(r'[A-Za-z_0-9]+').match(src, i)
            w = m.group(0)
            if w in MAP: changed += 1; w = MAP[w]
            out.append(w); i = m.end()
        else:
            out.append(c); i += 1
    return ''.join(out), changed

for f in sys.argv[1:]:
    s = open(f, encoding='utf-8', errors='surrogateescape').read()
    t, k = convert(s)
    if k:
        open(f, 'w', encoding='utf-8', errors='surrogateescape').write(t)
        print(f, k)
