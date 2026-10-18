#!/bin/bash
# MP.1/MP.2 live gate: runs --net-selftest and --mp-selftest against a local ewb-server `edenserver`.
#   port/tools/mp-live-gate.sh <path-to-edenserver> [path-to-eden_native]   (default native/build/eden_native)
# The scratch dir (world, players, control socket, log) is TESTERS/mp-live/ in the repo.
set -e
SRV=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
HERE=$(cd "$(dirname "$0")/../.." && pwd)
EXE=${2:-$HERE/native/build/eden_native}
PORT=${EDEN_MP_PORT:-27931}
DIR=$HERE/TESTERS/mp-live
rm -rf "$DIR" && mkdir -p "$DIR" && cd "$DIR"
"$SRV" --port "$PORT" --world mp.model > server.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do
  (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null && break
  sleep 0.1
done
"$EXE" --net-selftest --server=127.0.0.1:"$PORT"
"$EXE" --mp-selftest --server=127.0.0.1:"$PORT"
