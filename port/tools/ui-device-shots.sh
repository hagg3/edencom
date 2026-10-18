#!/bin/sh
# I.0: shoot the UI at each device preset into TESTERS/iphone-ui/<preset>/.
# Usage: port/tools/ui-device-shots.sh [preset ...]   (default: all presets)
# MP.6: "desktop" is the default window (no --device), so "iphone16 ipadair2 desktop" is the three sizes.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$ROOT/native/build/eden_native"
[ -x "$BIN" ] || { echo "build native first: $BIN missing" >&2; exit 1; }
PRESETS=${*:-iphone16 iphone16promax iphonese ipadair2 ipadpro11}
for p in $PRESETS; do
  d="$ROOT/TESTERS/iphone-ui/$p"; mkdir -p "$d"
  if [ "$p" = desktop ]; then dev=""; else dev="--device=$p"; fi
  "$BIN" --shot="$d/shot" $dev 2>&1 | grep -E "eden-display|eden-shot\]" | grep -v "player\|geom" | tee "$d/log.txt"
done
