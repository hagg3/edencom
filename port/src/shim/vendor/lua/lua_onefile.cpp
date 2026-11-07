// Emod (K.4-1, WORKING/mod-scripting-plan-2026-11-03.md S1): the vendored Lua 5.4 compiled as ONE C++
// translation unit. Not upstream's file (port/tools/vendor-lua.sh leaves it alone).
//
// C++, not C: ldo.c then raises Lua errors with `throw` (LUAI_THROW), so an error raised through a C
// function written in C++ (Classes/ModScript.cpp) unwinds its destructors instead of longjmp-ing over
// them. Every Lua symbol therefore has C++ linkage: include "lua.h"/"lauxlib.h"/"lualib.h" directly, never
// lua.hpp (its extern "C" would not link).
//
// Only the core and the libraries a sandboxed mod may open are vendored: base, coroutine, string, table,
// math, utf8. io, os, package and debug are not in the tree (no linit.c either), so nothing can open them.
// The order is upstream onelua.c's.
// libstdc++ pulls <cmath> -> <bits/stl_iterator_base_funcs.h> (a function named `next`) lazily from the C
// headers lcode.c includes, after llex.c has #defined `next(ls)`: include the C++ headers first.
#include <algorithm>
#include <iterator>
#include <cmath>
#include <cfloat>
#include <climits>
#include <clocale>
#include <csetjmp>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cctype>
#include "lprefix.h"
#include "lzio.c"
#include "lctype.c"
#include "lopcodes.c"
#include "lmem.c"
#include "lundump.c"
#include "ldump.c"
#include "lstate.c"
#include "lgc.c"
#include "llex.c"
#include "lcode.c"
#include "lparser.c"
#include "ldebug.c"
#include "lfunc.c"
#include "lobject.c"
#include "ltm.c"
#include "lstring.c"
#include "ltable.c"
#include "ldo.c"
#include "lvm.c"
#include "lapi.c"
#include "lauxlib.c"
#include "lbaselib.c"
#include "lcorolib.c"
#include "lmathlib.c"
#include "lstrlib.c"
#include "ltablib.c"
#include "lutf8lib.c"
