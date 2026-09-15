#!/usr/bin/env python3
"""compare-worlds.py — Phase N Stage 3's cross-platform save comparison.

WHAT THIS CAN AND CANNOT ASSERT, because the first version of this check asserted the wrong
thing and went red on its own premise.

Each platform's job CREATES ITS OWN world and saves it, so the three files are three separate
sessions. Two things in them are legitimately not reproducible, and neither is a portability
defect:

  * the header's `home` and `yaw` come from the engine's RANDOM SPAWN (arc4random), and `pos` is
    wherever the player settled;
  * a small part of the column data. Measured on one machine, same binary, back-to-back runs:
    134 bytes out of 44,976 differ, in a single ~870-byte span inside one column record. The world
    simulates while the harness settles, and that simulation takes random ticks.

So "the three files are byte-identical" was never achievable, on any platform, and a check that
demands it is measuring the dice. What IS invariant, and what this asserts:

  * SIZE. Same format version, same column-record size, same creature block, same directory
    layout -> same number of bytes. A platform that got SIZEOF_COLUMN or the header's padding
    wrong shows up here immediately.
  * level_seed, directory_offset and version -- the three header fields that decide how a loader
    walks the file. A difference here means one platform wrote a file another cannot read.
  * THE COLUMNINDEX DIRECTORY, byte for byte. It is the part of a .eden file that is read to EOF
    to find every column, it is 16-byte structs of int/int/unsigned long long, and it is exactly
    where an LLP64 `long` or a padding disagreement would land. This is the strongest claim
    available from bytes alone.

Everything else is REPORTED, not asserted -- with the differing offsets, so that a change in the
shape of the difference (a whole column, rather than a few dozen bytes in one) is visible to a
human even though it cannot be a threshold.

The real interchange proof is not here at all: it is that each platform LOADS the world Linux
wrote and reports Linux's geometry checksum for it. See the "read the world Linux wrote" gate.

    usage: compare-worlds.py <ref.eden> <other.eden> [<other.eden> ...]
"""

import struct
import sys

HEADER_BYTES = 192


def load(path):
    with open(path, "rb") as f:
        b = f.read()
    if len(b) < HEADER_BYTES:
        fail(f"{path}: {len(b)} bytes, shorter than a 192-byte WorldFileHeader")
    # Classes/FileManager.h: int level_seed; Vector pos; Vector home; float yaw;
    # unsigned long long directory_offset; char name[50]; int version; ...
    (level_seed,) = struct.unpack_from("<i", b, 0)
    (directory_offset,) = struct.unpack_from("<Q", b, 32)
    (version,) = struct.unpack_from("<i", b, 92)
    name = b[40:90].split(b"\0")[0].decode("utf-8", "replace")
    return {"path": path, "bytes": b, "size": len(b), "level_seed": level_seed,
            "directory_offset": directory_offset, "version": version, "name": name}


def fail(msg):
    print(f"::error::{msg}")
    print(f"FAIL: {msg}")
    sys.exit(1)


def diff_offsets(a, b):
    return [i for i in range(min(len(a), len(b))) if a[i] != b[i]]


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    worlds = [load(p) for p in argv[1:]]
    for w in worlds:
        print(f"{w['path']}: {w['size']} bytes, name={w['name']!r} version={w['version']} "
              f"level_seed={w['level_seed']} directory_offset={w['directory_offset']}")

    ref = worlds[0]
    if not 0 < ref["directory_offset"] <= ref["size"]:
        fail(f"{ref['path']}: directory_offset {ref['directory_offset']} is outside the file "
             f"({ref['size']} bytes) -- this file is not loadable by anything")

    bad = False
    for w in worlds[1:]:
        label = f"{ref['path']} vs {w['path']}"
        for field in ("size", "level_seed", "directory_offset", "version"):
            if ref[field] != w[field]:
                print(f"::error::{label}: {field} differs ({ref[field]} vs {w[field]})")
                bad = True
        if bad:
            continue

        d0 = ref["directory_offset"]
        if ref["bytes"][d0:] != w["bytes"][d0:]:
            off = diff_offsets(ref["bytes"][d0:], w["bytes"][d0:])
            print(f"::error::{label}: the ColumnIndex directory differs at +{d0}, "
                  f"{len(off)} byte(s), first at +{d0 + off[0]}")
            bad = True
        else:
            print(f"OK: {label} -- size, level_seed, directory_offset, version and the "
                  f"{ref['size'] - d0}-byte ColumnIndex directory all match")

        # Reported, never asserted. See this file's header for why.
        off = diff_offsets(ref["bytes"], w["bytes"])
        inhdr = [o for o in off if o < HEADER_BYTES]
        rest = [o for o in off if o >= HEADER_BYTES]
        print(f"    for the record: {len(off)} of {ref['size']} bytes differ "
              f"({len(inhdr)} in the header, {len(rest)} in the world data)")
        if inhdr:
            print(f"      header offsets: {inhdr[:32]}{' ...' if len(inhdr) > 32 else ''}")
        if rest:
            print(f"      data spans {rest[0]}..{rest[-1]}")

    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
