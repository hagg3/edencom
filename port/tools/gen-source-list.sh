#!/usr/bin/env bash
# Regenerate tools/engine-sources.txt — the authoritative list of the 81 files the
# Eden.xcodeproj "Sources" build phase compiles, resolved to real paths relative to the
# PARENT tree (one level above port/). Per ../CLAUDE.md, ONLY these files are built;
# ignore "Classes *.zip", RunAndGun/, Eden_mac/.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"          # parent source tree
pbx="$root/Eden.xcodeproj/project.pbxproj"
out="$here/engine-sources.txt"

awk '/Begin PBXSourcesBuildPhase/,/End PBXSourcesBuildPhase/' "$pbx" \
  | grep -oE '[A-Za-z0-9_+.~-]+\.(mm|cpp|c|m) in Sources' | sed 's/ in Sources//' | sort -u \
  | while read -r f; do
      if   [ -f "$root/$f" ];         then echo "$f"
      elif [ -f "$root/Classes/$f" ]; then echo "Classes/$f"
      else echo "MISSING:$f"
      fi
    done > "$out"

n=$(grep -c . "$out")
echo "wrote $out ($n files)"
grep MISSING "$out" && exit 1 || echo "all resolved"
