#!/usr/bin/env python3
"""QA.16 (audit A5-05): every gate mode the binary parses is a row of .github/scripts/gates.sh or a
`# MANUAL:` line there; and the table is honest.

Fails when
  1. a `--*-selftest` string literal in port/src/harness/main.cpp (or --p1-gate/--stage1/--leak-probe/
     --save-roundtrip/--mp-replay) is neither used by a gates.sh row nor listed MANUAL/EXTERNAL;
  2. a MANUAL line names a mode main.cpp does not parse (stale) or one that is also a row (then it is not manual);
  3. native.yml runs a selftest mode of its own that gates.sh does not know (a second copy of the list).

usage: check-gate-coverage.py          (run from native/ or the repo root; reads paths relative to this file)
"""
import os
import re
import sys

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
gates = open(os.path.join(root, ".github/scripts/gates.sh"), encoding="utf-8").read()
main_cpp = open(os.path.join(root, "port/src/harness/main.cpp"), encoding="utf-8").read()
workflow = open(os.path.join(root, ".github/workflows/native.yml"), encoding="utf-8").read()

MODE = r"--(?:[a-z0-9]+-)*[a-z0-9]+-selftest(?:-gl)?|--p1-gate|--stage1|--leak-probe|--save-roundtrip|--mp-replay"

# The modes main.cpp parses: string literals only (comments in main.cpp describe them too).
parsed = set(re.findall(r'"(' + MODE + r')[="]', main_cpp)) | set(re.findall(r'"(' + MODE + r')"', main_cpp))
if len(parsed) < 40:
    sys.exit("check-gate-coverage: found only %d modes in main.cpp; the pattern is stale" % len(parsed))

manual = {}
external = set()
rows = []
for line in gates.splitlines():
    m = re.match(r"#\s*(MANUAL|EXTERNAL):\s*(--\S+)\s+—\s+(.+)", line)
    if m:
        (manual.__setitem__(m.group(2), m.group(3)) if m.group(1) == "MANUAL" else external.add(m.group(2)))
        continue
    if line.lstrip().startswith("#"):
        continue
    rows.append(re.sub(r"\s#.*$", "", line))   # a trailing comment is not a use
used = set(re.findall(MODE, "\n".join(rows)))

bad = []
for mode in sorted(parsed):
    if mode not in used and mode not in manual and mode not in external:
        bad.append("%s: parsed by main.cpp but no gates.sh row and no `# MANUAL:` line" % mode)
for mode in sorted(manual):
    if mode not in parsed and mode + "=" not in main_cpp:
        bad.append("%s: listed MANUAL but main.cpp does not parse it" % mode)
    if mode in used:
        bad.append("%s: listed MANUAL but also a gates.sh row" % mode)

# 3. a mode on a native.yml `run:`/command line (not a comment) must be in the table or EXTERNAL;
#    the iOS simulator legs launch with --… args through simctl and are listed by their own gate names.
yml_modes = set()
for line in workflow.splitlines():
    s = line.strip()
    if s.startswith("#"):
        continue
    yml_modes |= set(re.findall(MODE, re.sub(r"\s#.*$", "", s)))
for mode in sorted(yml_modes):
    if mode not in used and mode not in external and mode not in ("--p1-gate", "--stage1", "--save-roundtrip"):
        bad.append("%s: run by native.yml but not in gates.sh (a second gate list)" % mode)

if bad:
    print("check-gate-coverage: FAIL")
    for b in bad:
        print("  " + b)
    sys.exit(1)
print("check-gate-coverage: ok (%d modes parsed, %d in the table, %d manual, %d external)"
      % (len(parsed), len(parsed & used), len(manual), len(external)))
