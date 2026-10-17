#!/bin/sh
# I.0: shoot the UI at each device preset into TESTERS/iphone-ui/<preset>/.
# Usage: port/tools/ui-device-shots.sh [preset ...]   (default: all presets)
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$ROOT/native/build/eden_native"
[ -x "$BIN" ] || { echo "build native first: $BIN missing" >&2; exit 1; }
PRESETS=${*:-iphone16 iphone16promax iphonese ipadair2 ipadpro11}
for p in $PRESETS; do
  d="$ROOT/TESTERS/iphone-ui/$p"; mkdir -p "$d"
  "$BIN" --shot="$d/shot" --device="$p" 2>&1 | grep -E "eden-display|eden-shot\]" | grep -v "player\|geom" | tee "$d/log.txt"
done
