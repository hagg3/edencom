#!/usr/bin/env python3
"""check-content-dump.py -- the K.2 content pin (WORKING/data-driven-content-plan-2026-11-01.md §6).

    check-content-dump.py LOG [LOG ...]

Each LOG is the output of `eden_native --content-dump` (other log lines around it are ignored, so an
iOS simulator EDEN_LOG works). The text between `[content-dump] BEGIN` and `[content-dump] END` must:
  1. hash to the binary's own `[content-dump] sha256=` line (the dump was not cut short),
  2. hash to PINNED below, and
  3. equal port/tools/fixtures/content/stock-dump.txt byte for byte (on a mismatch a unified diff
     names the rows that moved), and
  4. carry `## oracle` + `## tiles` sections that hash to ORACLE_PINNED.

The pin is every per-type table plus the Classes/BlockTraits.h oracle over all 256 type values. K.2-1
recorded it from the code as it stood (behaviour-neutral: stage1 pins, --scene-shot 0/66, --shot 0/53).
K.2-2 re-pinned PINNED on purpose (blockinfo bits 18-23 in the tables' `info` column; the `## sound sets`
and `## traits` sections appended) and made the oracle a view of those tables; ORACLE_PINNED is K.2-1's
oracle and never moves in K.2: it proves every trait table answers what the old per-site chains did.
Since K.2-3 the tables are what media/packs/eden/pack.json compiles to (Classes/ContentPack, loaded at
boot), and `port/tools/pack.py compile media/packs/eden --check LOG` must print the same text.
Re-pin PINNED only in a commit that changes block behaviour or a table on purpose, and say so there.
"""
import difflib
import hashlib
import os
import re
import sys

PINNED = "e9931771d94da118d79001af2b8a5b50298ff0040c7209a0850884f971090a27"
# K.2-1's `## oracle` + `## tiles` text (from `## oracle` up to the next section after `## tiles`).
ORACLE_PINNED = "0f22b291c563765a56691631f17a38c4246f130e5b66c8f9fdd9a4d3054265c6"
FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                       "port", "tools", "fixtures", "content", "stock-dump.txt")


def extract(path):
    with open(path, "rb") as f:
        data = f.read().replace(b"\r\n", b"\n")
    m = re.search(rb"^\[content-dump\] BEGIN\n(.*?)^\[content-dump\] END\n", data, re.S | re.M)
    if not m:
        return None, None
    s = re.search(rb"^\[content-dump\] sha256=([0-9a-f]{64})", data, re.M)
    return m.group(1), (s.group(1).decode() if s else None)


def oracle_section(text):
    """`## oracle` through the end of `## tiles` (the next section, or the end)."""
    a = text.find(b"## oracle\n")
    t = text.find(b"## tiles\n", a)
    if a < 0 or t < 0:
        return None
    b = text.find(b"\n## ", t)
    return text[a:] if b < 0 else text[a:b + 1]


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    with open(FIXTURE, "rb") as f:
        fixture = f.read().replace(b"\r\n", b"\n")   # Windows checkouts (core.autocrlf) turn the LF fixture into CRLF
    if hashlib.sha256(fixture).hexdigest() != PINNED:
        print("FAIL: %s does not hash to the pin (edit both together)" % FIXTURE)
        return 1
    fo = oracle_section(fixture)
    if fo is None or hashlib.sha256(fo).hexdigest() != ORACLE_PINNED:
        print("FAIL: %s: the oracle section moved (K.2 keeps K.2-1's oracle byte-identical)" % FIXTURE)
        return 1
    bad = 0
    for path in argv[1:]:
        text, own = extract(path)
        if text is None:
            print("FAIL: %s: no [content-dump] BEGIN/END block" % path)
            bad += 1
            continue
        h = hashlib.sha256(text).hexdigest()
        if own != h:
            print("FAIL: %s: the dump hashes to %s but the binary printed %s (truncated log?)" % (path, h, own))
            bad += 1
            continue
        o = oracle_section(text)
        if o is None or hashlib.sha256(o).hexdigest() != ORACLE_PINNED:
            print("FAIL: %s: the oracle section moved (a trait table answers differently from K.2-1's code)" % path)
            bad += 1
        if h != PINNED:
            print("FAIL: %s: content dump %s != pinned %s" % (path, h, PINNED))
            diff = difflib.unified_diff(fixture.decode().splitlines(), text.decode(errors="replace").splitlines(),
                                        "stock-dump.txt", path, lineterm="", n=1)
            for i, line in enumerate(diff):
                if i >= 80:
                    print("... (diff cut at 80 lines)")
                    break
                print(line)
            bad += 1
            continue
        if o is not None and hashlib.sha256(o).hexdigest() == ORACLE_PINNED:
            print("OK: %s content dump %s (oracle %s)" % (path, h, ORACLE_PINNED[:16]))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
