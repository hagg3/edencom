#!/usr/bin/env bash
# UX.10-2 kill-point gate: for every n, run the tutorial world's first-launch install in a fresh Documents,
# _Exit (86) after its n-th staging step (EDEN_TEST_STAGE_KILL_AFTER, the K.0w-3 hook), then in a second process
# check what the kill left: never a half world, never the pref set without the world, and the next launch
# finishes the install exactly once. Ends at the first n the install survives.
#   port/tools/tutorial-crash-gate.sh <eden_native> <scratch dir (absolute)> [--tutorial-mutate=keyfirst]
set -u
B=${1:?usage: tutorial-crash-gate.sh <eden_native> <scratch dir> [extra args]}
W=${2:?usage: tutorial-crash-gate.sh <eden_native> <scratch dir> [extra args]}
shift 2
export SDL_AUDIO_DRIVER=dummy
mkdir -p "$W"
for a in "$@"; do case "$a" in --tutorial-mutate=*) echo "mutation: ${a#--tutorial-mutate=}";; esac; done
fails=0
for n in $(seq 1 40); do
  D="$W/n$n"
  rm -rf "$D"; mkdir -p "$D"
  EDEN_TEST_STAGE_KILL_AFTER=$n "$B" --tutorial-selftest --tutorial-phase=install --docs="$D" "$@" > "$D.install.log" 2>&1
  rc=$?
  if [ $rc -ne 86 ] && [ $rc -ne 0 ]; then
    echo "tutorial-crash: n=$n the install phase exited $rc"; tail -20 "$D.install.log"; fails=$((fails + 1))
  fi
  if "$B" --tutorial-selftest --tutorial-phase=verify --docs="$D" "$@" > "$D.verify.log" 2>&1; then
    what=$(grep -o 'test kill after step [0-9]* ([^)]*)' "$D.install.log" | head -1)
    echo "tutorial-crash: n=$n ${what:-no kill (the install completed)}: verify PASS"
    rm -rf "$D"
  else
    echo "tutorial-crash: n=$n verify FAILED"; grep -E 'FAIL' "$D.verify.log" | head -20; fails=$((fails + 1))
  fi
  if [ $rc -eq 0 ]; then
    if [ $n -lt 4 ]; then echo "tutorial-crash: only $((n - 1)) kill points: the hook is not wired"; exit 1; fi
    break
  fi
done
if [ $fails -ne 0 ]; then echo "tutorial-crash: $fails FAILURE(S)"; exit 1; fi
echo "tutorial-crash: ALL PASS ($((n - 1)) kill points)"
