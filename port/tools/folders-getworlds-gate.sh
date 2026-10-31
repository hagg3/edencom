#!/usr/bin/env bash
# K.0w-3a: Get Worlds commit kill loop (folders-getworlds-gate.sh <eden_native> <abs scratch dir>): kill --browser-selftest
# after staging step n, then a list scan must leave no .partial and no half world (every world.emod is readable-or-absent).
B=$1; W=$2; fails=0
export SDL_AUDIO_DRIVER=dummy
for n in 1 2 3 4 5 6; do
  D=$W/g$n; rm -rf "$D"; mkdir -p "$D"
  EDEN_TEST_STAGE_KILL_AFTER=$n "$B" --headless --browser-selftest --docs="$D" > "$D.run.log" 2>&1; rc=$?
  "$B" --headless --base-selftest --docs="$D" >/dev/null 2>&1
  parts=$(find "$D/Worlds" -maxdepth 1 -name '.*.partial' 2>/dev/null | wc -l | tr -d ' ')
  dl=$(find "$D/Worlds" -name download -o -name 'download-*' 2>/dev/null | wc -l | tr -d ' ')
  if [ "$parts" = 0 ] && [ "$dl" = 0 ]; then echo "getworlds-gate n=$n ok (rc=$rc)"; else echo "getworlds-gate n=$n FAIL parts=$parts dl=$dl"; fails=$((fails+1)); fi
  [ "$rc" = 0 ] && break
done
echo "getworlds-gate fails=$fails"; exit $fails
