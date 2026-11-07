#!/bin/sh
# K.4-1: copies the Lua 5.4 sources Emod compiles into port/src/shim/vendor/lua/ from an UNPACKED official
# release tree (https://www.lua.org/ftp/, check the tarball's sha256 against the page first).
#   port/tools/vendor-lua.sh /path/to/lua-5.4.9
# Only the core and the libraries a mod script may use are copied: base, coroutine, string, table, math, utf8.
# io, os, package (loadlib), debug, linit and the lua/luac programs are never vendored, so no build can
# open them by accident (WORKING/mod-scripting-plan-2026-11-03.md S1). lua_onefile.cpp (Emod's, not
# upstream's) compiles them all as ONE C++ translation unit: Lua errors become C++ exceptions, so a C
# function in Classes/ModScript.cpp that raises one still runs its destructors.
set -e
SRC="$1"
[ -f "$SRC/src/lua.h" ] || { echo "usage: $0 <lua-5.4.x tree>"; exit 1; }
grep -q '#define LUA_VERSION_MINOR_N	4' "$SRC/src/lua.h" || grep -q 'LUA_VERSION_MINOR	"4"' "$SRC/src/lua.h" || { echo "not Lua 5.4"; exit 1; }
OUT="$(cd "$(dirname "$0")/../.." && pwd)/port/src/shim/vendor/lua"
mkdir -p "$OUT"
for f in lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm \
         lundump lvm lzio lauxlib lbaselib lcorolib lmathlib lstrlib ltablib lutf8lib; do
  cp "$SRC/src/$f.c" "$OUT/"
done
for h in "$SRC"/src/*.h; do cp "$h" "$OUT/"; done
# The licence is the MIT notice at the end of lua.h; ship it beside the sources (NOTICE names it too).
sed -n '/^\/\*\*\*\*\*\*\*\*\*\*/,$p' "$SRC/src/lua.h" | sed -e '1d' -e '/^\*\*\*\*/,$d' | sed 's/^\* \{0,1\}//' > "$OUT/LICENSE"
rm -f "$OUT/lualib.h.orig" "$OUT/linit.c"
echo "wrote $OUT ($(ls "$OUT" | wc -l | tr -d ' ') files)"
