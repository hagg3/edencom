#!/bin/bash
# FT.15a: run one Emod watt leg on the iPad over SSH (plan §4.2/§4.3). Usage:
#   power-leg.sh <leg> <secs> [emod args...]        e.g. power-leg.sh Eclear 480 --power-exp=clear
#   FT15_WORLD=FT15 FT15_POSE="65537.5 36 65524.5 180 8" FT15_OUT=TESTERS/ft15/r1 FT15_DOCS=<container>/Documents
# Launches with --live-cmds (+ --power-log=5), opens the world, parks at a fixed pose, starts the on-device
# sampler (power-sampler.sh), waits, then fetches <leg>.csv and the run log into $FT15_OUT.
set -e
here=$(cd "$(dirname "$0")" && pwd)
leg=$1; secs=$2; shift 2
SSH="ssh -o ConnectTimeout=10 ipad"
: "${FT15_DOCS:?set FT15_DOCS to the Emod container Documents dir (see memory ipad-device-testing)}"
D=$FT15_DOCS; world=${FT15_WORLD:-FT15}; pose=${FT15_POSE:-65537.5 36 65524.5 180 8}
out=${FT15_OUT:-TESTERS/ft15/r1}; mkdir -p "$out"
settle=${FT15_SETTLE:-90}
$SSH 'killall Emod' >/dev/null 2>&1 || true; sleep 2
$SSH "rm -f $D/emod-last-run.log; : > $D/live.txt; echo '--live-cmds=$D/live.txt --power-log=5 $*' > $D/eden-args.txt; uiopen --bundleid io.github.hagg3.emod" >/dev/null 2>&1
for i in $(seq 30); do sleep 2; $SSH "grep -q 'live\] tailing' $D/emod-last-run.log" 2>/dev/null && break; done
printf 'open %s\ntp %s\nwait %d\nquit\n' "$world" "$pose" $(( (secs + settle + 60) * 1000 )) | $SSH "cat >> $D/live.txt"
sleep "$settle"
FT15_DIR=$(dirname "$D")/ft15 "$here/power-sampler.sh" "$leg" "$secs"
sleep $((secs + 10))
scp -q ipad:"$(dirname "$D")/ft15/$leg.csv" "$out/$leg.csv"; scp -q ipad:"$D/emod-last-run.log" "$out/$leg.log"
$SSH 'killall Emod' >/dev/null 2>&1 || true
echo "$leg: $(wc -l < "$out/$leg.csv") samples -> $out"
