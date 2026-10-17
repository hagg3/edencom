#!/bin/sh
# C.5 gate 6 (dev-console-plan.md §8): --live-cmds drives a scripted file through the legacy verbs and
# `/` console lines; the [live] output must equal port/tools/fixtures/live-cmds/expected.txt.
# usage: live-cmds-gate.sh <eden_native> <scratch dir>   (SDL dummy video/audio from the environment)
set -u
BIN=$1; WORK=$2
FIX="$(cd "$(dirname "$0")" && pwd)/fixtures/live-cmds"
rm -rf "$WORK" && mkdir -p "$WORK/docs"
: > "$WORK/feed.txt"
"$BIN" --live-cmds="$WORK/feed.txt" --docs="$WORK/docs" > "$WORK/run.log" 2>&1 &
PID=$!
sleep 10
cat "$FIX/gate.txt" >> "$WORK/feed.txt"
i=0
while kill -0 $PID 2>/dev/null && [ $i -lt 120 ]; do sleep 1; i=$((i+1)); done
if kill -0 $PID 2>/dev/null; then kill $PID; echo "live-cmds gate: no quit within 130 s"; cat "$WORK/run.log"; exit 1; fi
grep -ao '\[live\] .*' "$WORK/run.log" | grep -v '^\[live\] tailing' | tr -d '\r' > "$WORK/got.txt"
if ! diff "$FIX/expected.txt" "$WORK/got.txt"; then echo "live-cmds gate: output differs"; exit 1; fi
echo "live-cmds gate: ok"
