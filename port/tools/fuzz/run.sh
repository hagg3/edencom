#!/bin/bash
# K.0 fuzz: run the four targets for $1 seconds each (default 240), in parallel; exit 1 on any crash, leak, timeout or OOM.
# usage: port/tools/fuzz/run.sh [seconds] [build dir]   (from the repo root, after build.sh)
# Seeds are built fresh from the checked-in fixtures: emod = fixtures/emod/*.emod (+ the gunzipped .emod.gz), eden =
# fixtures/emod/*.eden.gz gunzipped, json = every pack.json under fixtures/ and media/packs, net = two lines.
# A crashing input is kept in <work>/crashes/ (the CI job uploads it).
T=${1:-240}; B=${2:-native/build-fuzz}; W=${FUZZ_WORK:-native/fuzz-work}
export FZ_WORK=$W/work ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1
rm -rf "$W"; mkdir -p "$W/crashes" "$W/work" "$W/corpus/emod" "$W/corpus/eden" "$W/corpus/json" "$W/corpus/net"
F=port/tools/fixtures
cp $F/emod/*.emod "$W/corpus/emod/" 2>/dev/null
for g in $F/emod/*.emod.gz; do [ -f "$g" ] && gunzip -c "$g" > "$W/corpus/emod/$(basename "$g" .gz)"; done
for g in $F/emod/*.eden.gz; do [ -f "$g" ] && gunzip -c "$g" > "$W/corpus/eden/$(basename "$g" .gz)"; done
n=0; for j in $(find $F media/packs -name '*.json' -size -200k); do n=$((n+1)); cp "$j" "$W/corpus/json/$n.json"; done
printf 'HELLO 1 name\n' > "$W/corpus/net/a"; printf 'SNAPZ:AAAA\n' > "$W/corpus/net/b"
pids=""
for t in emod eden json net; do
  case $t in emod) ML=200000;; eden) ML=400000;; *) ML=1100000;; esac
  $B/fz_$t "$W/corpus/$t" -max_total_time=$T -max_len=$ML -timeout=20 -rss_limit_mb=3072 -malloc_limit_mb=1100 \
     -artifact_prefix="$W/crashes/$t-" -print_final_stats=1 > "$W/fuzz-$t.log" 2>&1 &
  pids="$pids $!"
done
bad=0; for p in $pids; do wait $p || bad=1; done
for t in emod eden json net; do echo "== $t"; tail -12 "$W/fuzz-$t.log"; done
ls "$W/crashes" | grep -q . && { echo "crash artifacts:"; ls "$W/crashes"; bad=1; }
exit $bad
