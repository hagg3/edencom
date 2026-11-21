#!/usr/bin/env bash
# gates.sh — THE gate list (QA.16, audit A5-04/05/10/11). Every CI job that runs the native binary's
# selftests takes its list from the table at the bottom of this file; native.yml has no copy of it.
#
#   gates.sh <job>          run the job's gates (from native/; B = the binary, set by the caller or guessed)
#   gates.sh --list <job>   print what <job> would run, in order (diff it in a PR)
#   gates.sh --jobs         the job names
#
# Jobs: linux windows macos sanitize tsan.   Environment:
#   B           the binary (default ./build/eden_native[.exe])
#   PY          python (default python3; windows: python)
#   GATE_JOBS   how many `pgate`/`pmutant` rows run at once (default 1 = serial; CI sets 3)
#   GATE_FAILFAST=1   stop at the first red gate (default: run them all, list the red ones at the end)
#
# ROW FORMS (platform spec = comma list of job names; `all` = linux,windows,macos,
# `every` = all + sanitize; a `-name` removes one):
#   gate    <name> <platforms> <cmd…>             run through run_gate (log gate-<name>.log, backtrace on a crash)
#   pgate   <name> <platforms> <cmd…>             same, but may run in parallel with its neighbours
#   mutant  <name> <platforms> <regex> <cmd…>     run_mutant: must exit 1, print `mutation: <suffix>` and a FAIL line matching <regex>
#   pmutant …                                     parallel mutant
# `@DOCS@` in a command is replaced by a fresh empty directory gate-docs/<slug of the name> (every run
# gets its own --docs: --stage1 creates-or-LOADS a world by name).
# A non-parallel row first waits for the queued parallel ones, so a row that reads an earlier row's log
# (check-content-dump.py reads gate-content-dump.log) is a plain `gate`. Rows that measure time (background,
# audio, net, mp), bind ports, or are heavy (radius 16, leak probe) are never parallel.
#
# Every `--*-selftest` mode main.cpp parses must be a row here or a `# MANUAL:` line below:
# check-gate-coverage.py (a gate itself) fails otherwise.

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$HERE/$(basename "${BASH_SOURCE[0]}")"
. "$HERE/run-gate.sh"

JOB=""; LIST=0; QUEUED=""
case "${1:-}" in
  --jobs)   echo "linux windows macos sanitize tsan"; exit 0 ;;
  --list)   LIST=1; JOB="${2:-}" ;;
  --run-queued) QUEUED="$2"; JOB="${3:-}" ;;
  -*)       echo "usage: gates.sh [--list] <job> | --jobs" >&2; exit 2 ;;
  *)        JOB="${1:-}" ;;
esac
case "$JOB" in
  linux|windows|macos|sanitize|tsan) ;;
  *) echo "gates.sh: job must be one of: $(bash "$SELF" --jobs)" >&2; exit 2 ;;
esac
case "$JOB" in windows) : "${PY:=python}" ;; *) : "${PY:=python3}" ;; esac
case "$JOB" in windows) : "${B:=./build/eden_native.exe}" ;; *) : "${B:=./build/eden_native}" ;; esac
: "${GATE_JOBS:=1}"
export B PY JOB GATE_JOBS

on() {   # on <spec>: does the current job match?
  local hit=1 t
  for t in $(printf '%s' "$1" | tr ',' ' '); do
    case "$t" in
      all)   case "$JOB" in linux|windows|macos) hit=0 ;; esac ;;
      every) case "$JOB" in linux|windows|macos|sanitize) hit=0 ;; esac ;;
      -*)    [ "${t#-}" = "$JOB" ] && hit=1 ;;
      *)     [ "$t" = "$JOB" ] && hit=0 ;;
    esac
  done
  return $hit
}

slug() { printf '%s' "$1" | tr -c 'A-Za-z0-9' '-'; }

FAILED=""
QDIR="gate-queue"
QN=0

# exec_gate gate|mutant <name> [<regex>] <cmd…>: make the docs dir, substitute @DOCS@, run.
exec_gate() {
  local kind="$1" name="$2" regex="" a d args=()
  shift 2
  [ "$kind" = mutant ] && { regex="$1"; shift; }
  d="$PWD/gate-docs/$(slug "$name")"
  for a in "$@"; do
    case "$a" in
      *@DOCS@*) rm -rf "$d"; mkdir -p "$d"; args+=("${a//@DOCS@/$d}") ;;
      *)        args+=("$a") ;;
    esac
  done
  if [ "$kind" = mutant ]; then run_mutant "$name" "$regex" "${args[@]}"; else run_gate "$name" "${args[@]}"; fi
}

# A gate that is a shell function (the broken-stock-pack refusal).
broken_stock_pack() {
  local d="$PWD/gate-docs/broken-bundle"
  rm -rf "$d"; mkdir -p "$d/packs/eden"
  cp ../port/tools/fixtures/content/packs/blocks/pack.json "$d/packs/eden/"
  if $B --content-dump --bundle="$d" > "$d/out.log" 2>&1; then
    echo "::error::a broken stock pack did not stop the boot"; cat "$d/out.log"; return 1; fi
  grep -q 'FATAL: the stock content pack did not load' "$d/out.log" || { cat "$d/out.log"; return 1; }
}
# --save-fail-selftest must also print the player-invisible half.
save_fail_line() {
  grep -q '^emod: save failed: ' gate-save-fail-selftest.log || { echo "::error::--save-fail-selftest printed no 'emod: save failed:' line"; return 1; }
}

if [ -n "$QUEUED" ]; then   # a queued row, run by xargs below: output to <file>.out, status to <file>.rc
  rc=0
  eval "exec_gate $(cat "$QUEUED")" > "$QUEUED.out" 2>&1 || rc=$?
  echo "$rc" > "$QUEUED.rc"
  exit 0
fi

flush() {   # run the queued parallel rows, then print their output in table order
  [ "$QN" -gt 0 ] || return 0
  ls "$QDIR"/*.cmd | xargs -P "$GATE_JOBS" -I{} bash "$SELF" --run-queued {} "$JOB"
  local f rc
  for f in "$QDIR"/*.cmd; do
    cat "$f.out"
    rc=$(cat "$f.rc" 2>/dev/null || echo 99)
    if [ "$rc" != 0 ]; then FAILED="$FAILED
  $(cut -c1-70 "$f" | head -n1)"; fi
  done
  rm -rf "$QDIR"; QN=0
}

row() {   # row <parallel 0|1> gate|mutant <name> <platforms> [<regex>] <cmd…>
  local par="$1" kind="$2" name="$3" plats="$4"
  shift 4
  on "$plats" || return 0
  if [ "$LIST" = 1 ]; then printf '%-7s%s %s\n' "$kind" "$([ "$par" = 1 ] && echo '[p]' || echo '   ')" "$name"; return 0; fi
  if [ "$par" = 1 ] && [ "$GATE_JOBS" -gt 1 ]; then
    mkdir -p "$QDIR"; QN=$((QN + 1))
    printf '%q ' "$kind" "$name" "$@" > "$(printf '%s/%04d.cmd' "$QDIR" "$QN")"
    return 0
  fi
  flush
  if ! exec_gate "$kind" "$name" "$@"; then
    FAILED="$FAILED
  $name"
    if [ "${GATE_FAILFAST:-0}" = 1 ]; then echo "::error::gate '$name' failed (GATE_FAILFAST)"; exit 1; fi
  fi
}
gate()    { local n="$1" p="$2"; shift 2; row 0 gate   "$n" "$p" "$@"; }
pgate()   { local n="$1" p="$2"; shift 2; row 1 gate   "$n" "$p" "$@"; }
mutant()  { local n="$1" p="$2"; shift 2; row 0 mutant "$n" "$p" "$@"; }
pmutant() { local n="$1" p="$2"; shift 2; row 1 mutant "$n" "$p" "$@"; }

# ---------------------------------------------------------------------------------------------------
# MANUAL / EXTERNAL modes (parsed by check-gate-coverage.py; the reason is the text after the dash).
#   A MANUAL mode must NOT be a row; an EXTERNAL one is run by a native.yml step the table cannot express.
# MANUAL: --cmds-selftest — needs the gitignored 3-bit adder world (--world= --at); run on the Mac, see native/README.md
# MANUAL: --convert-prompt-selftest — needs a 64z .eden fixture and --convert-answer=upgrade|keep|cancel; columnsToUpdate is zeroed since REL.3, so it could become a row
# MANUAL: --signs-selftest — needs a world with signs (--world= --at); run on the Mac
# MANUAL: --render-selftest-gl — needs a window (the GL leg of --render-selftest); run on the Mac
# MANUAL: --gl-selftest — alias of --render-selftest since G.5b-7
# EXTERNAL: --net-live-selftest — the real HTTP backend; informational step in native.yml (depends on remote servers)
# ---------------------------------------------------------------------------------------------------

if [ "$LIST" != 1 ]; then
  echo "gates: job=$JOB B=$B PY=$PY GATE_JOBS=$GATE_JOBS"
  mkdir -p gate-docs
fi

# ---- repository checks (no binary) -------------------------------------------------------------
# G.2b-0 (plan D14): the ObjC surface may only shrink; a converted file may not regain any.
gate "objc surface"       linux   $PY -I ../port/tools/objc-surface.py --check
gate "dead exports"       macos   $PY -I ../port/tools/dead-exports.py   # QA.21: no stripped eden_* export without a call site (relinks with -map; Mac only)
# LG.7: the shipped .edm creature models are exactly pod2edm.py's conversion of the .pod art.
gate "pod2edm check"      linux   $PY -I ../port/tools/pod2edm.py --check
# SRC.4: the Srclike movement reference model still prints its golden vectors.
gate "srclike pm ref"     linux   $PY -I ../port/tools/srclike-pm-ref.py --check
# G.5b-7: no live GL call outside the renderer.   FT.4: no non-ASCII bytes in UI string literals.
gate "gl call gate"       linux   $PY -I ../port/tools/gl-call-gate.py
gate "ui string gate"     linux   $PY -I ../port/tools/ui-string-gate.py
# QA.26: the public reading set links only public files and leaks no /Users/ path.
gate "linkcheck"          linux   $PY -I ../.github/scripts/linkcheck.py
# WS.3: the project website builds; links, images, weight budget and the publish script's pages.yml hold.
gate "site check"         linux   $PY -I ../site/build.py --check
# QA.16: every --*-selftest main.cpp parses is a row here or a MANUAL line (A5-05).
gate "gate coverage"      linux   $PY -I ../.github/scripts/check-gate-coverage.py
# QA.16 (A5-11): docs/console-reference.md is what the binary prints (C.10).
gate "console reference"  linux   $PY ../port/tools/console-reference.py $B --check
# The offline .eden 64z <-> 256z converter (pure byte surgery, no engine).
gate "eden-convert test"  linux   node ../port/tools/eden-convert-test.js

# ---- boot and geometry -------------------------------------------------------------------------
gate "boot (--p1-gate)"   all  $B --p1-gate
# A FRESH --docs PER RUN: --stage1 creates-or-LOADS a world by name, so a 64z save left by the previous
# command is silently reused by the 256z one. H.2: --shadows=off pins the pair to Off whatever the
# setting's default becomes; the shadows leg asserts geom is unchanged and a pinned `full`.
gate "geometry 64z"       every   $B --headless --stage1 --shadows=off --docs=@DOCS@
gate "geometry 256z"      every   $B --headless --stage1 --shadows=off --height=256 --docs=@DOCS@
gate "stage1 checksums"   every   $PY ../.github/scripts/check-stage1.py gate-geometry-64z.log gate-geometry-256z.log
gate "geometry 64z shadows" all   $B --headless --stage1 --shadows=classic --docs=@DOCS@
gate "stage1 shadows"     all     $PY ../.github/scripts/check-stage1.py --shadows gate-geometry-64z-shadows.log

# ---- the deterministic selftests (parallel block) ----------------------------------------------
pgate "base selftest"     every   $B --base-selftest --docs=@DOCS@
pgate "registry selftest" every   $B --registry-selftest --docs=@DOCS@
pgate "json selftest"     every   $B --json-selftest
pgate "pack selftest"     every   $B --pack-selftest=../port/tools/fixtures/content/packs
pgate "packs selftest"    every   $B --packs-selftest --docs=@DOCS@
pgate "mods selftest"     every   $B --mods-selftest --docs=@DOCS@
pgate "hotbar selftest"   every   $B --hotbar-selftest --docs=@DOCS@
# UX.10-1: the bundled tutorial world loads with the layout's signs and command blocks, every script parses, the
# spawn stands on solid ground (reads port/tools/tutorial/manifest.txt).
pgate "tutorial selftest" every   $B --tutorial-selftest --docs=@DOCS@
# UX.10-2: the install (once per install, delete, Restore) in-process; the kill-point loop through the install
# (never a half world, never the key without the world); a mutation leg for each (no sidecar; key before the commit).
gate "tutorial crash loop" all    bash ../port/tools/tutorial-crash-gate.sh $B @DOCS@
pmutant "tutorial-nomarker" all  'the first scan installs one world with the marker' $B --tutorial-selftest --tutorial-mutate=nomarker --docs=@DOCS@
# UX.10-3: the in-world hints (each step completes in order in all three input modes; no card without the marker or in
# a session) and its mutation leg (hints keyed on the world's name must trip the no-marker leg).
pmutant "tutorial-hintsbyname" all 'a world without the marker shows no card' $B --tutorial-selftest --tutorial-mutate=hintsbyname --docs=@DOCS@
mutant "tutorial-keyfirst" all   'the key is never set without the world' bash ../port/tools/tutorial-crash-gate.sh $B @DOCS@ --tutorial-mutate=keyfirst
pgate "lua selftest"      every   $B --lua-selftest --docs=@DOCS@
pgate "folders selftest"  every   $B --headless --folders-selftest --docs=@DOCS@
pgate "emod selftest"     every   $B --emod-selftest --emod-fixtures=../port/tools/fixtures/emod --emod-work=@DOCS@
# K.6-5a (C-4): compaction dropping reserved record types again must FAIL the reserved section.
pmutant "emod-dropunknown" all 'the newest record of each reserved type survives compaction' $B --emod-selftest --emod-quick --emod-mutate=dropunknown --emod-fixtures=../port/tools/fixtures/emod --emod-work=@DOCS@
pmutant "emod-stripalways" all 'the Include copy of an export keeps record 13' $B --emod-selftest --emod-quick --emod-mutate=stripalways --emod-fixtures=../port/tools/fixtures/emod --emod-work=@DOCS@
pgate "classic selftest"  all $B --classic-selftest --docs=@DOCS@
pgate "look selftest"     all     $B --look-selftest --docs=@DOCS@
pgate "fly selftest"      all     $B --fly-selftest --docs=@DOCS@
pgate "srclike selftest"  all     $B --srclike-selftest --docs=@DOCS@   # move reads ../port/tools/fixtures/srclike-pm/vectors.txt
pgate "recolor selftest"  all     $B --recolor-selftest --docs=@DOCS@
pgate "display selftest"  all     $B --display-selftest --docs=@DOCS@
pgate "ui selftest"       all $B --ui-selftest --docs=@DOCS@
pgate "convert64 selftest" all    $B --convert64-selftest --docs=@DOCS@
pgate "props selftest"    all     $B --props-selftest --docs=@DOCS@
pgate "camera selftest"   all     $B --camera-selftest --docs=@DOCS@
pgate "settings selftest" all     $B --settings-selftest --docs=@DOCS@
pgate "creature selftest" all     $B --creature-selftest --docs=@DOCS@   # hashes pinned for macOS arm64; structural elsewhere
pgate "render selftest"   all,sanitize $B --render-selftest --docs=@DOCS@
pgate "mp replay"         all     $B --mp-replay=../port/tools/fixtures/mp/replay-1.lines --docs=@DOCS@
# The orphans (A5-05), measured on the Mac 2026-11-13: all pass with no arguments, 0-14 s.
pgate "load-fail selftest" all    $B --load-fail-selftest --docs=@DOCS@
pgate "keybind selftest"  all     $B --keybind-selftest --docs=@DOCS@
pgate "fit selftest"      all     $B --fit-selftest --docs=@DOCS@
pgate "fit selftest iphone16" all $B --fit-selftest --device=iphone16 --docs=@DOCS@   # UX.10-5: the tutorial card at a notched phone
pgate "fit selftest ipadair2" all  $B --fit-selftest --device=ipadair2 --docs=@DOCS@
pgate "newblocks selftest" all    $B --newblocks-selftest --docs=@DOCS@
pgate "empty-bit selftest" all    $B --empty-bit-selftest --docs=@DOCS@
pgate "photo selftest"    all     $B --photo-selftest --docs=@DOCS@
pgate "trailer selftest"  all     $B --trailer-selftest --docs=@DOCS@
# Each --*-mutate puts one fixed bug back and must FAIL that check (run_mutant, QA.11).
pmutant "look-lastonly"   all  'step 0: the trace sees nothing lost'              $B --look-selftest --look-mutate=lastonly --docs=@DOCS@
pmutant "look-int"        all  'step 2: a 0\.3-point/frame drag turns every frame' $B --look-selftest --look-mutate=int --docs=@DOCS@
pmutant "look-enginepts"  all  'step 3: a 60-window-point swipe'                  $B --look-selftest --look-mutate=enginepts --docs=@DOCS@
pmutant "look-noflush"    all  'step 4: released mid-motion, the flush'           $B --look-selftest --look-mutate=noflush --docs=@DOCS@
pmutant "look-noslop"     all  'step 0: the trace counts 91 events'               $B --look-selftest --look-mutate=noslop --docs=@DOCS@
pmutant "look-noclear"    all  'step 6: a 40-point drag turns past the slop'      $B --look-selftest --look-mutate=noclear --docs=@DOCS@
pmutant "look-nodecay"    all  'step 7: glide on'                                 $B --look-selftest --look-mutate=nodecay --docs=@DOCS@
pmutant "look-unlatched"  all  'step 9: an out-and-back'                          $B --look-selftest --look-mutate=unlatched --docs=@DOCS@
pmutant "look-multi"      all  'step 8: a second world finger'                    $B --look-selftest --look-mutate=multi --docs=@DOCS@
pmutant "look-clearlatch" all  'step 8b: the look keeps turning'                  $B --look-selftest --look-mutate=clearlatch --docs=@DOCS@
# fly-move: the wall check, not the cruise one: under the mutation noclip cruises at Fly's speed, which rides the
# wall-clock frame EMA (97 b/s on a fast Mac, 58 on a CI runner, inside the NOCLIP_SPEED 60 tolerance).
pmutant "fly-move"        all  'noclip passes a wall Fly stops at'                $B --fly-selftest --fly-mutate=move --docs=@DOCS@
pmutant "lua-hook"        all  'a 300k-iteration loop is paused'                  $B --lua-selftest --lua-mutate=hook --docs=@DOCS@
pmutant "lua-sandbox"     all  'the sandbox: no io/os'                            $B --lua-selftest --lua-mutate=sandbox --docs=@DOCS@
pmutant "lua-loadearly"   all  '`load` fires exactly once'                         $B --lua-selftest --lua-mutate=loadearly --docs=@DOCS@
pmutant "lua-caperror"    all  'reaching the 64-coroutine cap'                    $B --lua-selftest --lua-mutate=caperror --docs=@DOCS@
pmutant "lua-usefilter"   all  '`use` on stone, none of `burn`'                   $B --lua-selftest --lua-mutate=usefilter --docs=@DOCS@
pmutant "lua-signnocache" all  'SignRenderer shows a Lua-set sign'            $B --lua-selftest --lua-mutate=signnocache --docs=@DOCS@
pmutant "lua-storenotdirty" all 'save, reopen: every stored value is back exactly' $B --lua-selftest --lua-mutate=storenotdirty --docs=@DOCS@
pmutant "lua-dialognoqueue" all 'two packs asking in one frame' $B --lua-selftest --lua-mutate=dialognoqueue --docs=@DOCS@
pmutant "lua-dialognoclamp" all 'a 6000-byte dialog body' $B --lua-selftest --lua-mutate=dialognoclamp --docs=@DOCS@
pmutant "lua-nogen"       all  'a stale handle to a reused slot'                  $B --lua-selftest --lua-mutate=nogen --docs=@DOCS@
pmutant "lua-nonfinite"  all  'h:setvel raises for a NaN'                        $B --lua-selftest --lua-mutate=nonfinite --docs=@DOCS@
pmutant "lua-toastflood" all 'eden.toast: the HUD status line' $B --lua-selftest --lua-mutate=toastflood --docs=@DOCS@
pmutant "lua-leaveaftersave" all 'a `leave` handler.s block and eden.store key are in the exit save' $B --lua-selftest --lua-mutate=leaveaftersave --docs=@DOCS@
pmutant "ui-settingsinpack" all 'reinstalling the pack.s zip keeps its settings file' $B --ui-selftest --ui-mutate=settingsinpack --docs=@DOCS@
pmutant "mods-anyid"      all  'K\.8-3: a zip whose id is not'                     $B --mods-selftest --mods-mutate=anyid --docs=@DOCS@
pmutant "classic-tables"  all  '1\.x columns regenerate byte for byte'            $B --classic-selftest --classic-mutate=tables --docs=@DOCS@
pmutant "classic-trunc"   all  '1\.x columns regenerate byte for byte'            $B --classic-selftest --classic-mutate=trunc --docs=@DOCS@
pmutant "classic-noborder" all 'profile hash matches its pin'                     $B --classic-selftest --classic-mutate=noborder --docs=@DOCS@
pmutant "props-nogen"     all  'Change terrain appends a newer GENERATOR'         $B --props-selftest --props-mutate=nogen --docs=@DOCS@
pmutant "props-nolamp"    all  'picking New Dawn writes record 14'                $B --props-selftest --props-mutate=nolamp --docs=@DOCS@
pmutant "weather-lowatmo" all  'all Off: 600 frames draw only the stock'            $B --weather-selftest=off --weather-mutate=lowatmo --docs=@DOCS@
pmutant "evict-noceiling" all  'ceiling layer: blocks at y = H-1'                  $B --evict-selftest --evict-mutate=noceiling --docs=@DOCS@
pmutant "mp-noevictcommit" all 'leg 2: walking out and back'                      $B --mp-replay=../port/tools/fixtures/mp/replay-1.lines --mp-mutate=noevictcommit --docs=@DOCS@
pmutant "mp-nolandhook"   all  'leg 1: the window equals'                         $B --mp-replay=../port/tools/fixtures/mp/replay-1.lines --mp-mutate=nolandhook --docs=@DOCS@
pmutant "mp-noreconcile"  all  'leg 3: a refused edit'                            $B --mp-replay=../port/tools/fixtures/mp/replay-1.lines --mp-mutate=noreconcile --docs=@DOCS@
pmutant "mp-echo"         all  'leg 6: applying a peer'                           $B --mp-replay=../port/tools/fixtures/mp/replay-1.lines --mp-mutate=echo --docs=@DOCS@

# ---- content parity: the binary's dump vs the independent Python reference ---------------------
gate "content dump"       every   $B --content-dump
gate "content check"      every   $PY ../.github/scripts/check-content-dump.py gate-content-dump.log
gate "pack.py stock"      linux   $PY ../port/tools/pack.py compile ../media/packs/eden --check gate-content-dump.log
gate "pack.py fixtures"   linux   $PY ../port/tools/pack.py fixtures ../port/tools/fixtures/content/packs
# K.2-4 mod packs, the Wonder Blocks pack, K.3 requires/conflicts: the same parity in both validators.
gate "content dump mods"  all     $B --content-dump --packs=../port/tools/fixtures/packs/emod_example
gate "pack.py mods"       linux   $PY ../port/tools/pack.py compile ../port/tools/fixtures/packs/emod_example --check gate-content-dump-mods.log
gate "content dump wonders" all   $B --content-dump --packs=../port/tools/fixtures/packs/wonders
gate "pack.py wonders"    linux   $PY ../port/tools/pack.py compile ../port/tools/fixtures/packs/wonders --check gate-content-dump-wonders.log
gate "content dump resolve" all   $B --content-dump --packs=../port/tools/fixtures/content/mods-resolve
gate "pack.py resolve"    linux   $PY ../port/tools/pack.py compile ../port/tools/fixtures/content/mods-resolve --check gate-content-dump-resolve.log
gate "content dump usable" all   $B --content-dump --packs=../port/tools/fixtures/content/mod-usable
gate "pack.py usable"     linux   $PY ../port/tools/pack.py compile ../port/tools/fixtures/content/mod-usable --check gate-content-dump-usable.log
gate "lua api stubs"      linux   $PY ../port/tools/check-lua-stubs.py $B ../docs/eden-api.lua
gate "check-pack"         linux   $PY ../port/tools/check-pack-gate.py $B ../port/tools/fixtures
# K.2-3: a broken or missing stock pack is fatal (exit 1), never a half-filled table.
gate "broken stock pack"  linux,macos broken_stock_pack
# K.1-4: a 256-high run adds Convert to 64z (the palette and flag survive the cut).
gate "registry selftest 256" linux $B --registry-selftest --height=256 --docs=@DOCS@

# ---- the serial gates: time, ports, SDL, or heavy ---------------------------------------------
gate "input selftest"     all     $B --input-selftest
gate "gamepad selftest"   all     $B --gamepad-selftest
# Stage 4.3: SDL finger events at the engine's own reported HUD rect (the translator is shared).
gate "touch selftest"     all     $B --touch-selftest
gate "audio selftest"     all     $B --audio-selftest
gate "convert64 crosscheck" linux,macos node ../port/tools/convert64-crosscheck.js $B @DOCS@
gate "background selftest" all    $B --background-selftest --world=bg --docs=@DOCS@
# QA.5: a save under RLIMIT_FSIZE is reported and loses nothing; the `emod: save failed:` line is the
# player-invisible half. Windows SKIPs the leg, so it is not a row there.
gate "save-fail selftest" linux,macos $B --save-fail-selftest --docs=@DOCS@
gate "save-fail line"     linux,macos save_fail_line
mutant "save-fail-exit"   linux,macos 'Save & Quit on a failed save stays in the world' $B --save-fail-selftest --save-fail-mutate=exit --docs=@DOCS@
mutant "save-fail-stream" linux,macos 'a walk does not stream over the unsaved column' $B --save-fail-selftest --save-fail-mutate=stream --docs=@DOCS@
# ROADMAP 5.9: Get Worlds end to end over OFFLINE fixtures it writes itself.
gate "browser selftest"   all     $B --browser-selftest --docs=@DOCS@
gate "folders crash loop" all bash ../port/tools/folders-crash-gate.sh $B @DOCS@
gate "net selftest"       all     $B --net-selftest
gate "mp selftest"        all     $B --mp-selftest
gate "bundled map"        all,sanitize $B --bundled-map-selftest --bundled-eden=../Eden.eden --bundled-emod=../Eden.emod
gate "evict selftest"     all,sanitize $B --evict-selftest --docs=@DOCS@
gate "ring light selftest" all    $B --ring-light-selftest --docs=@DOCS@
gate "light selftest"     all     $B --light-selftest --light-selfcheck --docs=@DOCS@
gate "evict selftest r16" all     $B --evict-selftest --radius=16 --docs=@DOCS@
gate "ring light selftest r16" all $B --ring-light-selftest --radius=16 --docs=@DOCS@
# LM.1: lamp light profiles. Bytes per profile, a live switch each way == a fresh load; the ring walk at New
# Dawn and at the cap (17); R=18 must break the 3x3/5x5 neighbourhood rebuild (Lighting.h LIGHT_RADIUS_MAX).
pgate "light modes selftest" all  $B --light-selftest=modes --docs=@DOCS@
pgate "ring light newdawn" all    $B --ring-light-selftest --lamp-light=newdawn --docs=@DOCS@
pgate "ring light r17"    all     $B --ring-light-selftest --lamp-light=newdawn --lamp-radius=17 --docs=@DOCS@
pmutant "light-r18" all 'the walked light equals a full sweep of the same window' $B --ring-light-selftest --light-mutate=r18 --docs=@DOCS@
gate "shadow selftest"    all     $B --shadow-selftest --docs=@DOCS@
gate "shadow selftest r16" all    $B --shadow-selftest --radius=16 --docs=@DOCS@
# SH.3: the sun-shadow map's life cycle (null backend): off = absent, dirty rules, night/storm, the Shadows replace.
pgate "sunshadow selftest" all   $B --sunshadow-selftest --docs=@DOCS@
# C.1/C.2/QA.9: the command-block language as a console line, the Settings bridge, the day/night flip.
gate "console selftest"   every   $B --console-selftest --docs=@DOCS@
gate "weather selftest"   every   $B --weather-selftest --docs=@DOCS@
mutant "console-anchor"   all  'the anchor is the cell under the player.s feet'  $B --console-selftest --console-mutate=anchor --docs=@DOCS@
mutant "console-noreg"    all  'gate 3: every Settings row'                      $B --console-selftest --console-mutate=noreg --docs=@DOCS@
mutant "console-flipburst" all 'QA\.9 a night flip streams in'                   $B --console-selftest --console-mutate=flipburst --docs=@DOCS@
mutant "console-unbound"  all  'QA\.19 every settings value row is bound'         $B --console-selftest --console-mutate=unbound --docs=@DOCS@
mutant "console-noflag"   all  'QA\.20 every edit verb is refused'                 $B --console-selftest --console-mutate=noflag --docs=@DOCS@
# C.5 gate 6: --live-cmds drives legacy verbs and `/` console lines; output pinned.
gate "live-cmds gate"     all     ../port/tools/live-cmds-gate.sh $B @DOCS@
# QA.2: worlds whose header spawn/home/yaw or a creature hold NaN/1e30/inf must load (the sanitize job
# creates ./hostile-venv first: emod.py needs zstandard).
gate "hostile worlds"     sanitize sh ../port/tools/hostile-world-gate.sh "$PWD/$B" @DOCS@ "$PWD/hostile-venv/bin/python3"
# UX.10-1: media/Tutorial.emod and its manifest are byte-identical to a rebuild from layout.py + copy.txt + Eden.emod,
# and every refusal fires on a planted bad input (a copy edit without a rebuild fails here). Same venv: zstandard.
gate "tutorial world"     sanitize "$PWD/hostile-venv/bin/python3" -I ../port/tools/tutorial.py --check
# The stage's leak obligation (Windows reports process commit charge in the live-bytes slot).
gate "leak probe (torture, same 64z)" all $B --leak-probe --docs=@DOCS@
# X.32: enter/leave cycles (weather on, the example pack toggled so the block atlases re-upload): R::'s texture
# ledger sees no stale destroy or use and the UI icon atlas keeps its name. Headless here; windowed (a Mac or a
# Windows desktop) it also reads the atlas back and runs the bind-cache leg, whose mutant is
# `--rejoin-mutate=elide` (needs a window, so not a row).
gate "rejoin selftest"    every   $B --headless --rejoin-selftest=4 --docs=@DOCS@
mutant "rejoin-doubledestroy" all 'cycle 1/2: no stale texture destroy or use'   $B --headless --rejoin-selftest=2 --rejoin-mutate=doubledestroy --docs=@DOCS@
gate "save round-trip"    every   $B --headless --save-roundtrip --world=ci --docs=@DOCS@

# ---- TSan (halt_on_error=1; the job exports TSAN_OPTIONS) --------------------------------------
gate "tsan stage1"        tsan    $B --headless --stage1 --shadows=off --docs=@DOCS@
gate "tsan leak probe 3"  tsan    $B --leak-probe=3 --docs=@DOCS@
gate "tsan evict selftest" tsan   $B --evict-selftest --docs=@DOCS@
gate "tsan browser selftest" tsan $B --browser-selftest --docs=@DOCS@
gate "tsan mods selftest" tsan    $B --mods-selftest --docs=@DOCS@
gate "tsan net selftest"  tsan    $B --net-selftest
gate "tsan mp selftest"   tsan    $B --mp-selftest

flush
if [ "$LIST" = 1 ]; then exit 0; fi
if [ -n "$FAILED" ]; then
  echo "::error::$JOB: red gates:$FAILED"
  exit 1
fi
echo "gates: $JOB all green"
