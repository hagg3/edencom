#!/bin/bash
# B.5b-1c gate 1, light leg: on a world full of lightboxes, the walked window == a fresh load at the
# end point in geometry, chunkstate AND light (hash, lit voxels, sum), and the walk streamed
# (bulk_reloads unchanged) and rebuilt light per landed column (rebuilds grew).
# usage: port/tools/ring-light-gate.sh HEIGHT OUTDIR [BINARY] [EXTRA ARGS...]
# B.5b-2: EDEN_WINDOW_RADIUS=N in the environment runs it at window radius N (the harness pins 9 otherwise).
#   OUTDIR under TESTERS/ (gitignored). EXTRA ARGS go to every run, e.g. --light-mutate=nosync|nomark
#   (gate 6: each must FAIL).
H=$1; OUT=$2; B=${3:-$(cd "$(dirname "$0")/../.." && pwd)/native/build/eden_native}; shift $(( $# < 3 ? $# : 3 )); EXTRA="$*"
X0=65536.5; Y=60; Z0=65536.5
fresh_docs() { rm -rf "$1"; mkdir -p "$1"; echo 1 > "$1/.migrated-from-documents"; }   # never migrate real saves in
mkdir -p "$OUT"
fresh_docs "$OUT/prep"
$B --headless --stage1 --height=$H --world=g --at=$X0,$Y,$Z0 --light-lattice=23 --frames=600 --docs="$OUT/prep" > "$OUT/prep.log" 2>&1 \
  || { echo "prep FAILED (see $OUT/prep.log)"; exit 99; }
grep "light-lattice" "$OUT/prep.log"
walks="px:160,0 nx:-160,0 pz:0,160 nz:0,-160 diag:113,113 back:160,0,-96,0 zig:64,0,0,64,-64,0,0,-64"
for w in $walks; do
  n=${w%%:*}; d=${w#*:}
  ex=$X0; ez=$Z0; IFS=, read -ra v <<< "$d"
  for ((i=0;i<${#v[@]};i+=2)); do ex=$(echo "$ex+${v[i]}"|bc); ez=$(echo "$ez+${v[i+1]}"|bc); done
  ( rm -rf "$OUT/$n-walk" "$OUT/$n-fresh"; cp -R "$OUT/prep" "$OUT/$n-walk"; cp -R "$OUT/prep" "$OUT/$n-fresh"
    $B --headless --stage1 --height=$H --world=g --at=$X0,$Y,$Z0 --walk=$d --docs="$OUT/$n-walk" $EXTRA > "$OUT/$n-walk.log" 2>&1
    $B --headless --stage1 --height=$H --world=g --at=$ex,$Y,$ez --docs="$OUT/$n-fresh" $EXTRA > "$OUT/$n-fresh.log" 2>&1 ) &
done
wait
light() { grep -E "$1\.light" "$2" | sed -E 's/.*"hash":"([0-9a-f]+)","lit":([0-9]+),"sum":([0-9]+).*/\1 lit=\2 sum=\3/'; }
field() { grep -E "$1" "$2" | sed -E "s/.*\"$3\":([0-9]+).*/\1/"; }
fails=0
for w in $walks; do
  n=${w%%:*}
  a=$(grep -E "walked\.(checksum|chunkstate)" "$OUT/$n-walk.log" | sed -E 's/.*(checksum|chunkstate) //' | sed -E 's/"selfcheck.*//')
  b=$(grep -E "world(64|256)\.(checksum|chunkstate)" "$OUT/$n-fresh.log" | sed -E 's/.*(checksum|chunkstate) //' | sed -E 's/"selfcheck.*//')
  la=$(light walked "$OUT/$n-walk.log"); lb=$(light "world$H" "$OUT/$n-fresh.log")
  end=$(grep "walked.end" "$OUT/$n-walk.log" | sed 's/.*end //')
  b0=$(field "world$H.geometry" "$OUT/$n-walk.log" bulk_reloads); b1=$(field "walked.geometry" "$OUT/$n-walk.log" bulk_reloads)
  r0=$(field "world$H.light" "$OUT/$n-walk.log" rebuilds); r1=$(field "walked.light" "$OUT/$n-walk.log" rebuilds)
  ok=1; [ -n "$a" ] && [ "$a" == "$b" ] || ok=0; [ -n "$la" ] && [ "$la" == "$lb" ] || ok=0
  [ "$b0" == "$b1" ] || ok=0; [ "$r1" -gt "$r0" ] 2>/dev/null || ok=0
  case "$la" in *" lit=0 "*) ok=0;; esac   # a light hash over no light proves nothing
  if [ $ok == 1 ]; then echo "$n PASS end=$end light=[$la] rebuilds+$((r1-r0)) bulk $b0->$b1"
  else fails=$((fails+1)); echo "$n FAIL end=$end rebuilds $r0->$r1 bulk $b0->$b1"
    [ "$a" == "$b" ] || { echo " walk : $a"; echo " fresh: $b"; }
    echo " light walk : $la"; echo " light fresh: $lb"; fi
done
exit $fails
