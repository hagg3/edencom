#!/bin/sh
# WS.4: make the website's screenshots from the binary's deterministic shot modes.
# Usage: port/tools/site-shots.sh [--keep]    (macOS: needs sips; build native first)
# Raw BMPs go to TESTERS/ws4/{atmo,menu}/; sized JPEGs land in site/static/img/.
# Scene frames: --atmo-shot at 1280x720 -> 960x540 JPEG q75 (hero: 1280x720 q80).
# Menu/HUD frames: --shot (1136x640) -> kept at full size, JPEG q80, named ui-<page>.jpg.
# No size cap: the pages show them at 640 CSS px wide, so these are 1.5-2x for sharp screens.
# Override with SHOT_W/SHOT_H/SHOT_Q (scenes) if you want other numbers.
# Rerun is byte-identical for scene frames and the static menu pages (fixed etime, pinned RNG).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN="$ROOT/native/build/eden_native"
OUT="$ROOT/site/static/img"
RAW="$ROOT/TESTERS/ws4"
[ -x "$BIN" ] || { echo "build native first: $BIN missing" >&2; exit 1; }
command -v sips >/dev/null || { echo "sips not found (macOS only)" >&2; exit 1; }
export SDL_AUDIO_DRIVER=dummy
mkdir -p "$RAW/atmo" "$RAW/menu" "$OUT"
(cd "$RAW/atmo" && rm -f ./*.bmp && "$BIN" --atmo-shot=a- --window=1280x720 >atmo.log 2>&1)
(cd "$RAW/menu" && rm -f ./*.bmp && "$BIN" --shot=s- >shot.log 2>&1)

# conv SRC DEST W H Q
W=${SHOT_W:-960}; H=${SHOT_H:-540}; Q=${SHOT_Q:-75}
conv() { sips -s format jpeg -s formatOptions "$5" -z "$4" "$3" "$1" --out "$2" >/dev/null; }

# Scene frames (name = a--<name>.bmp -> <name>.jpg), 960x540 unless listed below.
for n in water-noon snow-noon ambient-canopy base-site clear-dusk-up rain-noon storm-noon \
         clear-dawn-level under-noon ambient-fireflies halos-near clear-night-up; do
  conv "$RAW/atmo/a--$n.bmp" "$OUT/$n.jpg" $W $H $Q
done
conv "$RAW/atmo/a--clear-noon-level.bmp" "$OUT/clear-noon-level.jpg" 720 1280 80   # hero (2x for Retina)

# Menu pages, 1136x640, kept full size. These are byte-stable across runs.
# menu, hud, picker-*, console draw a live world (its terrain/thumbnail varies per session), so
# they are made too but differ by a few JPEG bytes on every run: expect those five to show as changed in git.
for n in home settings-keys settings-video mods mp-list newworld \
         menu hud picker-blocks picker-colors console; do
  conv "$RAW/menu/s--$n.bmp" "$OUT/ui-$n.jpg" 640 1136 80
done
du -ch "$OUT"/*.jpg | tail -1
