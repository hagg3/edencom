#!/bin/bash
# FT.15a: device-side watt meter (plan §4.1). Usage: power-sampler.sh <leg> <secs> [ssh-host=ipad] [process-grep=Emod.app/Emod]
# Starts a nohup loop ON the device (a Wi-Fi hiccup cannot end it), every 5 s appending
#   t,amp_mA,mV,updateTime,extConn,battC,gpuDev,gpuRend,gpuTiler,cpu
# to Documents/ft15/<leg>.csv (the container's Documents, found via the running app's dir or $FT15_DOCS).
# Refuses to start when ExternalConnected=Yes (a charging leg measures nothing). Fetch with: scp host:<dir>/<leg>.csv
set -e
leg=$1; secs=$2; host=${3:-ipad}; proc=${4:-Emod.app/Emod}
[ -n "$leg" ] && [ -n "$secs" ] || { echo "usage: $0 <leg> <secs> [host] [proc]" >&2; exit 2; }
out=${FT15_DIR:-/var/mobile/Documents/ft15}
ssh -o ConnectTimeout=10 "$host" bash -s -- "$leg" "$secs" "$proc" "$out" <<'DEV'
leg=$1; secs=$2; proc=$3; out=$4
mkdir -p "$out"
if ioreg -rc AppleARMPMUCharger -w0 | grep -q '"ExternalConnected" = Yes'; then echo "REFUSED: plugged in (ExternalConnected=Yes)"; exit 3; fi
f="$out/$leg.csv"
echo "t,amp_mA,mV,updateTime,extConn,battC,gpuDev,gpuRend,gpuTiler,cpu" > "$f"
nohup bash -c '
  end=$(( $(date +%s) + '"$secs"' ))
  while [ $(date +%s) -lt $end ]; do
    p=$(ioreg -rc AppleARMPMUCharger -w0); a=$(ioreg -r -k PerformanceStatistics -w0)
    g(){ echo "$p" | sed -n "s/^ *| *\"$1\" = \(.*\)$/\1/p" | head -1; }
    u(){ echo "$a" | grep -o "\"$1 Utilization %\"=[0-9]*" | head -1 | cut -d= -f2; }
    amp=$(g InstantAmperage)   # raw u64; power-report.py folds >2^63 to negative
    cpu=$(ps -axo pcpu,comm | grep "'"$proc"'" | grep -v grep | head -1 | cut -c1-6)
    echo "$(date +%s),$amp,$(g Voltage),$(g UpdateTime),$(g ExternalConnected),$(g Temperature),$(u Device),$(u Renderer),$(u Tiler),$cpu" >> "'"$f"'"
    sleep 3
  done' >/dev/null 2>&1 &
echo "sampling $leg for ${secs}s -> $f"
DEV
