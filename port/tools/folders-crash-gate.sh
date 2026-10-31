#!/usr/bin/env bash
# K.0w-2 crash-injection gate (WORKING/world-folders-plan-2026-10-30.md §6): for every n, build the
# --folders-selftest fixture in a fresh Documents, run one adoption scan that _Exits (86) after its n-th
# mutating step (EDEN_TEST_ADOPT_KILL_AFTER), then run the verify phase (a clean scan, then: every world
# once with its bytes unchanged, no sidecar lost, no .migrating dir left, a further scan a no-op). Ends
# at the first n the scan survives (every step has then been killed once).
#   port/tools/folders-crash-gate.sh <eden_native> <scratch dir (absolute)>
set -u
B=${1:?usage: folders-crash-gate.sh <eden_native> <scratch dir>}
W=${2:?usage: folders-crash-gate.sh <eden_native> <scratch dir>}
export SDL_AUDIO_DRIVER=dummy
mkdir -p "$W"
fails=0
for n in $(seq 1 400); do
  D="$W/n$n"
  rm -rf "$D"; mkdir -p "$D"
  if ! "$B" --headless --folders-selftest --folders-phase=fixture --docs="$D" > "$D.fixture.log" 2>&1; then
    echo "folders-crash: n=$n fixture FAILED"; cat "$D.fixture.log"; exit 1
  fi
  EDEN_TEST_ADOPT_KILL_AFTER=$n "$B" --headless --folders-selftest --folders-phase=adopt --docs="$D" > "$D.adopt.log" 2>&1
  rc=$?
  if [ $rc -ne 86 ] && [ $rc -ne 0 ]; then
    echo "folders-crash: n=$n the adopt phase exited $rc"; tail -20 "$D.adopt.log"; fails=$((fails + 1))
  fi
  if "$B" --headless --folders-selftest --folders-phase=verify --docs="$D" > "$D.verify.log" 2>&1; then
    what=$(grep -o 'test kill after step [0-9]* ([^)]*)' "$D.adopt.log" | head -1)
    echo "folders-crash: n=$n ${what:-no kill (the scan completed)}: verify PASS"
    rm -rf "$D"
  else
    echo "folders-crash: n=$n verify FAILED"; grep -E 'FAIL|adopt:' "$D.verify.log" | head -20; fails=$((fails + 1))
  fi
  if [ $rc -eq 0 ]; then
    if [ $n -lt 10 ]; then echo "folders-crash: only $((n - 1)) kill points: the hook is not wired"; exit 1; fi
    break
  fi
done
if [ $fails -ne 0 ]; then echo "folders-crash: $fails FAILURE(S)"; exit 1; fi
echo "folders-crash: ALL PASS ($((n - 1)) kill points)"
