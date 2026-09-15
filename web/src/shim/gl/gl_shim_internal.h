// gl_shim_internal.h — private state shared ONLY between gl_fixed_function.cpp and
// gl_context_web.cpp (Phase N Stage 0.2 split, WORKING/native-migration-plan-2026-09-04.md).
// Not part of the public GL shim interface (gl_es1_shim.h) — nothing outside this directory
// includes it, and a future gl_context_native.cpp includes it the same way gl_context_web.cpp
// does.
#ifndef EDEN_GL_SHIM_INTERNAL_H
#define EDEN_GL_SHIM_INTERNAL_H

#include "gl_es1_shim.h"

namespace eden_gl_shim {
// Both owned (defined) by gl_fixed_function.cpp, both written by gl_context_web.cpp's
// context/drawable-size functions — see their declaration sites in gl_fixed_function.cpp for
// the full rationale (g_viewport is the real dynamic mirror; kPickViewport is the fixed
// engine-point-space rect GL_VIEWPORT actually answers with).
extern GLint g_viewport[4];
extern GLint kPickViewport[4];
}  // namespace eden_gl_shim

// Defined in gl_fixed_function.cpp (GROUP 2d, the draw path); gl_context_web.cpp calls it on
// context destroy/loss to drop GL object handles that belonged to the dead context.
extern "C" void eden_gl_shim_invalidate_gl_objects(void);

#endif  // EDEN_GL_SHIM_INTERNAL_H
