#!/bin/bash
# B.5b-1a gate 1: walked window == fresh load at the end point (geometry + chunkstate), and the walk
# streamed (bulk_reloads did not grow, ring_cols did).
# usage: port/tools/ring-walk-gate.sh HEIGHT OUTDIR [BINARY]   (OUTDIR under TESTERS/, it is gitignored)
# B.5b-2: EDEN_WINDOW_RADIUS=N in the environment runs it at window radius N (the harness pins 9 otherwise).
# H.4: CLASSIC=--classic=SEED[,SCALE4] in the environment makes the world a Classic (generated) one.
H=$1; OUT=$2; B=${3:-$(cd "$(dirname "$0")/../.." && pwd)/native/build/eden_native}
X0=65536.5; Y=60; Z0=65536.5
mkdir -p "$OUT"
walks="px:160,0 nx:-160,0 pz:0,160 nz:0,-160 diag:113,113 back:160,0,-96,0 zig:64,0,0,64,-64,0,0,-64"
for w in $walks; do
  n=${w%%:*}; d=${w#*:}
  ex=$X0; ez=$Z0; IFS=, read -ra v <<< "$d"
  for ((i=0;i<${#v[@]};i+=2)); do ex=$(echo "$ex+${v[i]}"|bc); ez=$(echo "$ez+${v[i+1]}"|bc); done
  ( rm -rf "$OUT/$n-walk" "$OUT/$n-fresh"; mkdir -p "$OUT/$n-walk" "$OUT/$n-fresh"
    for k in walk fresh; do echo 1 > "$OUT/$n-$k/.migrated-from-documents"; done   # never migrate real saves in
    $B --headless --stage1 --height=$H --world=g $CLASSIC --at=$X0,$Y,$Z0 --walk=$d --docs="$OUT/$n-walk" > "$OUT/$n-walk.log" 2>&1
    $B --headless --stage1 --height=$H --world=g $CLASSIC --at=$ex,$Y,$ez --docs="$OUT/$n-fresh" > "$OUT/$n-fresh.log" 2>&1 ) &
done
wait
fails=0
for w in $walks; do
  n=${w%%:*}
  a=$(grep -E "walked\.(checksum|chunkstate)" "$OUT/$n-walk.log" | sed -E 's/.*(checksum|chunkstate) //' | sed -E 's/"selfcheck.*//')
  b=$(grep -E "world(64|256)\.(checksum|chunkstate)" "$OUT/$n-fresh.log" | sed -E 's/.*(checksum|chunkstate) //' | sed -E 's/"selfcheck.*//')
  end=$(grep "walked.end" "$OUT/$n-walk.log" | sed 's/.*end //')
  b0=$(grep "world$H.geometry" "$OUT/$n-walk.log" | sed -E 's/.*"bulk_reloads":([0-9]+).*/\1/')
  b1=$(grep "walked.geometry" "$OUT/$n-walk.log" | sed -E 's/.*"bulk_reloads":([0-9]+).*/\1/')
  r0=$(grep "world$H.geometry" "$OUT/$n-walk.log" | sed -E 's/.*"ring_cols":([0-9]+).*/\1/')
  r1=$(grep "walked.geometry" "$OUT/$n-walk.log" | sed -E 's/.*"ring_cols":([0-9]+).*/\1/')
  ok=1; [ -n "$a" ] && [ "$a" == "$b" ] || ok=0; [ "$b0" == "$b1" ] || ok=0; [ "$r1" -gt "$r0" ] 2>/dev/null || ok=0
  if [ $ok == 1 ]; then echo "$n PASS end=$end ring_cols+$((r1-r0)) bulk $b0->$b1"
  else fails=$((fails+1)); echo "$n FAIL end=$end ring_cols $r0->$r1 bulk $b0->$b1"; echo " walk : $a"; echo " fresh: $b"; fi
done
exit $fails
