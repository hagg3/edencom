#!/usr/bin/env bash
# K.0w-3 staging crash gate (folders-stage-gate.sh <eden_native> <abs scratch dir> <64z .eden whose header name is convert64-xcheck, e.g. convert64-xcheck-src.eden from --convert64-selftest>): for every n, convert a fixture .eden (+ png + signs sidecar) with
# EDEN_TEST_STAGE_KILL_AFTER=n, then verify after a clean rerun: no .partial left, the source is either
# still loose (kill before the moves) or inside its folder; the converted world appears exactly once or
# not at all (kill before the commit); no file lost; all non-world files exactly once.
B=$1; W=$2; SRC=$3
export SDL_AUDIO_DRIVER=dummy EDEN_UPGRADE_256Z=1 EDEN_CONVERT_PROMPT=0
fails=0
for n in $(seq 1 30); do
  D=$W/s$n; rm -rf "$D"; mkdir -p "$D"
  cp "$SRC" "$D/Src.eden"; echo png > "$D/Src.eden.png"; printf x > "$D/signs_Src.eden.dat"
  EDEN_TEST_STAGE_KILL_AFTER=$n "$B" --headless --convert-prompt-selftest --docs="$D" --world=convert64-xcheck --convert-answer=upgrade > "$D.run.log" 2>&1
  rc=$?
  # recovery: a plain list scan sweeps staging dirs
  "$B" --headless --base-selftest --docs="$D" >/dev/null 2>&1
  EDEN_TEST_STAGE_KILL_AFTER= "$B" --headless --folders-selftest --folders-phase=adopt --docs="$D" >/dev/null 2>&1
  parts=$(find "$D/Worlds" -maxdepth 1 -name '.*.partial' 2>/dev/null | wc -l | tr -d ' ')
  worlds=$(find "$D/Worlds" -name world.emod 2>/dev/null | wc -l | tr -d ' ')
  src=$(find "$D" -name 'Src.eden' -o -name 'original.eden' | wc -l | tr -d ' ')
  sig=$(find "$D" -name 'signs*.dat' | wc -l | tr -d ' ')
  png=$(find "$D" -name '*.png' | wc -l | tr -d ' ')
  what=$(grep -o 'test kill after step [0-9]* ([^)]*)' "$D.run.log" | head -1)
  bad=""
  [ "$src" = 1 ] || bad="$bad src=$src"; [ "$sig" = 1 ] || bad="$bad sig=$sig"; [ "$png" = 1 ] || bad="$bad png=$png"
  [ "$parts" = 0 ] || bad="$bad parts=$parts"
  [ "$worlds" -le 1 ] || bad="$bad worlds=$worlds"
  [ "$rc" = 86 -o "$rc" = 0 ] || bad="$bad rc=$rc"
  if [ -n "$bad" ]; then echo "conv-gate n=$n ($what) FAIL:$bad parts=$parts"; fails=$((fails+1)); else echo "conv-gate n=$n ${what:-completed} ok (rc=$rc worlds=$worlds parts=$parts)"; fi
  [ "$rc" = 0 ] && break
done
echo "conv-gate fails=$fails"; exit $fails
