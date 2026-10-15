#!/usr/bin/env python3
"""check-stage1.py — assert Phase N Stage 3's success criterion 2.

Criterion 2 is the whole portability claim in one number: `--headless --stage1` on Linux and on
Windows must report the SAME geometry checksums the macOS leg already records.
Not "similar", not "plausible" — the same 64-bit hashes, because the mesher is deterministic and
every input to it is byte-defined by the save format.

    64z   geom 53d457fe6e6884e2   full 3b8e767dd12219ae
    256z  geom d53a88740f714762

(WORKING/phase-n-stage1-results-2026-09-05.md §1. Anything else is a stop, per the plan's kill
criteria — not a number to update here.)

Re-recorded ONCE, deliberately, by B.5b-1a (2026-10-11), which changed WHAT is meshed, not how: the
window is centred on the nearest chunk corner and its outer ring of columns is never meshed
(render-distance-runtime-plan-2026-10-11.md §3). The new values were taken on macOS native and
cross-checked against the (since retired, 2026-10-13) web build before being written here. The values
before it were 64z c26fa4fd2a5b2660 / 49b71a4e16afb125, 256z b19d3e33a8717c60.

Why a script rather than a `grep -q`: a grep for the expected hash passes silently when the line is
ABSENT for some other reason (the binary crashed before meshing, diagnostics were compiled out, the
world failed to load), and "the string is not there" is indistinguishable from "the string is not
there because nothing ran". This parses the line, requires it to exist, and prints what it actually
found next to what it wanted — which is the difference between a red build you can act on and one
you have to reproduce first.

H.2 (2026-10-14) adds a third leg, "geometry 64z shadows" (`--stage1 --shadows=classic`), and puts an
explicit `--shadows=off` on the two above so they never depend on the setting's default. Shadows
change vertex COLOURS only, so that leg's `geom` must still be the 64z value; its `full` is pinned
below (recorded on macOS native; Linux, Windows and the iOS simulator must match it -- a mismatch is
a stop, and fp contraction in the light path is the first suspect). A change to Classes/Shadow.h's
SHADOW_REACH_* constants or the shade formula re-pins it, deliberately and in the same commit.

    usage: check-stage1.py <64z-log> [<256z-log>]
           check-stage1.py --shadows <64z-shadows-log>
"""

import json
import re
import sys

EXPECTED_64 = {"geom": "53d457fe6e6884e2", "full": "3b8e767dd12219ae"}
EXPECTED_256_GEOM = "d53a88740f714762"
EXPECTED_64_SHADOWS = {"geom": EXPECTED_64["geom"], "full": "4c95e204f1827850"}   # Classic

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
    if argv[1] == "--shadows":
        if len(argv) < 3:
            print(__doc__)
            return 2
        check(argv[2], EXPECTED_64_SHADOWS, "64z shadows (Classic)", phase_want="world64")
        return 0
    check(argv[1], EXPECTED_64, "64z", phase_want="world64")
    if len(argv) > 2:
        check(argv[2], {"geom": EXPECTED_256_GEOM}, "256z", phase_want="world256")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
