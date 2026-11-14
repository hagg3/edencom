#!/bin/sh
# QA.2 gate (A3-02, A2-05): a world whose header spawn/home/yaw or first creature holds NaN, 1e30 or inf must
# load, play a frame and save, not crash. Each kind: make a world with --save-roundtrip, append a hostile
# batch with `emod.py hostile` (needs zstandard: pass a python that has it), run --save-roundtrip on it again.
# Built with -DEDEN_SANITIZE=ON, UBSan aborts on the unsanitised (int) casts; plain builds segfault ~2 in 3.
# usage: hostile-world-gate.sh <eden_native> <abs scratch dir> [python3]   (SDL dummy video/audio from the environment)
set -u
BIN=$1; WORK=$2; PY=${3:-python3}
EMOD="$(cd "$(dirname "$0")" && pwd)/emod.py"
rc=0
for kind in header-nan header-1e30 header-inf creature-nan; do
  D="$WORK/$kind"; rm -rf "$D" && mkdir -p "$D"
  "$BIN" --headless --save-roundtrip --world=hostile --docs="$D" > "$D/make.log" 2>&1 || { echo "hostile gate ($kind): world creation failed"; tail -5 "$D/make.log"; exit 1; }
  W=$(find "$D" -name '*.emod' | head -1)
  [ -n "$W" ] || { echo "hostile gate ($kind): no .emod written"; exit 1; }
  "$PY" "$EMOD" hostile "$W" "$kind" || { echo "hostile gate ($kind): emod.py hostile failed"; exit 1; }
  # Several loads: the creature top-up draws random offsets.
  bad=0
  for i in 1 2 3; do
    if ! "$BIN" --headless --save-roundtrip --world=hostile --docs="$D" > "$D/run$i.log" 2>&1; then
      echo "hostile gate ($kind, run $i): FAILED"; tail -15 "$D/run$i.log"; rc=1; bad=1; break
    fi
  done
  [ $bad -ne 0 ] && continue
  case $kind in header-*) grep -q 'header: spawn is not finite' "$D/run1.log" || { echo "hostile gate ($kind): sanitiser did not report"; rc=1; };; esac
  echo "hostile gate ($kind): ok"
done
exit $rc
