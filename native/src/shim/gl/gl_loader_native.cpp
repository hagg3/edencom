// gl_loader_native.cpp — resolves the 61 desktop-GL entry points gl_loader_native.h declares.
// Phase N Stage 3.2 (WORKING/phase-n-stage3plus-plan-2026-09-05.md). Non-Apple targets only;
// macOS links <OpenGL/gl3.h>'s symbols statically and never compiles this file.
//
// The file is two X-macro expansions over the one list in the header — define, then resolve — plus
// the guard described below. Nothing here is kept in sync by hand.

#if !defined(__APPLE__)

#include "gl_loader_native.h"

#include <SDL3/SDL.h>

#include <cstdio>

// *** THE GUARD THE HEADER PROMISES, and it is exact. ***
// The header carries two lists the preprocessor cannot derive from each other: the X-macro (which
// declares the pointers) and the `#define` renames (which redirect the call sites). An X-macro
// entry with NO matching rename is the dangerous direction — Linux's libGL exports the modern
// symbols, so such a call would quietly bind to the real function there and fail to link only on
// Windows, i.e. it would look fine on the leg that exists to be the control.
//
// This catches it per name, at compile time, with no third list: inside the macro `name` IS
// expanded (unlike the `##` and `#` uses below), so a renamed entry becomes the pointer VARIABLE
// and `sizeof` is fine, while an un-renamed one stays the FUNCTION — and `sizeof(function)` is
// ill-formed. For the 35 names SDL's header does not even prototype it is an undeclared
// identifier instead. Either way: a compile error naming the exact entry point.
#define EDEN_GL_CHECK_RENAMED(ret, name, params)                                              \
  static_assert(sizeof(name) == sizeof(eden_glfp_t_##name),                                   \
                "gl_loader_native.h declares this entry point but never #defines the rename " \
                "for it — add it to BOTH lists.");
EDEN_GL_ENTRY_POINTS(EDEN_GL_CHECK_RENAMED)
#undef EDEN_GL_CHECK_RENAMED

// Definitions. Null until eden_native_gl_load_entry_points() runs. `##` suppresses expansion, so
// these spell the real pointer names even with the renames in scope.
#define EDEN_GL_DEFINE(ret, name, params) eden_glfp_t_##name eden_glfp_##name = nullptr;
EDEN_GL_ENTRY_POINTS(EDEN_GL_DEFINE)
#undef EDEN_GL_DEFINE

extern "C" int eden_native_gl_load_entry_points(void) {
  int missing = 0;

// SDL_GL_GetProcAddress returns an `SDL_FunctionPointer` (a `void(*)(void)`), which is the correct
// portable spelling — round-tripping through `void*` is the thing the C standard actually forbids
// here, and MinGW warns about it. The cast to the specific signature is unavoidable and is what
// every GL loader does.
#define EDEN_GL_RESOLVE(ret, name, params)                                     \
  eden_glfp_##name = (eden_glfp_t_##name)SDL_GL_GetProcAddress(#name);         \
  if (!eden_glfp_##name) {                                                     \
    std::fprintf(stderr, "[eden-gl] missing entry point: %s\n", #name);        \
    missing++;                                                                 \
  }
  EDEN_GL_ENTRY_POINTS(EDEN_GL_RESOLVE)
#undef EDEN_GL_RESOLVE

  if (missing) {
    std::fprintf(stderr,
                 "[eden-gl] %d of %d GL entry points did not resolve. The context is not the "
                 "profile this renderer needs (it asks for 3.2+ core; see gl_context_native.cpp's "
                 "attribute block).\n",
                 missing, (int)EDEN_GL_ENTRY_POINT_COUNT);
  }
  return missing;
}

#endif  // !__APPLE__
