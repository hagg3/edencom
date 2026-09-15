#!/usr/bin/env python3
"""check-stage1.py — assert Phase N Stage 3's success criterion 2.

Criterion 2 is the whole portability claim in one number: `--headless --stage1` on Linux and on
Windows must report the SAME geometry checksums the macOS leg and the web build already record.
Not "similar", not "plausible" — the same 64-bit hashes, because the mesher is deterministic and
every input to it is byte-defined by the save format.

    64z   geom c26fa4fd2a5b2660   full 49b71a4e16afb125
    256z  geom b19d3e33a8717c60

(WORKING/phase-n-stage1-results-2026-09-05.md §1. Anything else is a stop, per the plan's kill
criteria — not a number to update here.)

Why a script rather than a `grep -q`: a grep for the expected hash passes silently when the line is
ABSENT for some other reason (the binary crashed before meshing, diagnostics were compiled out, the
world failed to load), and "the string is not there" is indistinguishable from "the string is not
there because nothing ran". This parses the line, requires it to exist, and prints what it actually
found next to what it wanted — which is the difference between a red build you can act on and one
you have to reproduce first.

    usage: check-stage1.py <64z-log> [<256z-log>]
"""

import json
import re
import sys

EXPECTED_64 = {"geom": "c26fa4fd2a5b2660", "full": "49b71a4e16afb125"}
EXPECTED_256_GEOM = "b19d3e33a8717c60"

# `[eden-stage1] world64.checksum {"chunks":1296,...,"geom":"...","full":"..."}`
LINE = re.compile(r"\[eden-stage1\]\s+(\S+)\.checksum\s+(\{.*\})\s*$")


def checksums(path):
    """Every <phase>.checksum object in a log, in order."""
    found = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if m:
                try:
                    found.append((m.group(1), json.loads(m.group(2))))
                except json.JSONDecodeError as e:
                    fail(f"{path}: could not parse the checksum line: {e}\n  {line.strip()}")
    return found


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def check(path, want, label, phase_want=None):
    found = checksums(path)
    if not found:
        fail(
            f"{path}: no '[eden-stage1] <phase>.checksum' line at all.\n"
            "  The run did not reach the mesher. Look further up the log for a load failure, and\n"
            "  check the build really was configured with -DEDEN_DIAGNOSTICS=ON (the probe that\n"
            "  prints this line is compiled out otherwise)."
        )
    phase, got = found[-1]
    if phase_want and phase != phase_want:
        fail(
            f"{path}: last checksum is phase '{phase}', expected '{phase_want}'.\n"
            "  --stage1 CREATES-OR-LOADS a world by name, so a stale save from an earlier run at a\n"
            "  different height is silently reused. Run with a fresh --docs=DIR."
        )
    bad = [(k, v, got.get(k)) for k, v in want.items() if got.get(k) != v]
    if bad:
        detail = "\n".join(f"    {k}: want {v}, got {g}" for k, v, g in bad)
        fail(
            f"{label} geometry differs from the recorded checksum ({path}, phase '{phase}'):\n"
            f"{detail}\n"
            "  This is Stage 3's kill criterion 2. Do not update the expected value here — the\n"
            "  point of the number is that it did not change. Find the difference.\n"
            "  FIRST thing to rule out: --stage1 creates-or-LOADS by name, so a stale save from an\n"
            "  earlier run at a different height is reused and reported under the new phase name.\n"
            "  Always run it against a fresh --docs=DIR."
        )
    print(f"OK: {label} {phase} " + " ".join(f"{k}={got[k]}" for k in want))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    check(argv[1], EXPECTED_64, "64z", phase_want="world64")
    if len(argv) > 2:
        check(argv[2], {"geom": EXPECTED_256_GEOM}, "256z", phase_want="world256")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
