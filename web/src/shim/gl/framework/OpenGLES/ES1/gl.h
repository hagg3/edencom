// Trampoline header. The engine includes `<OpenGLES/ES1/gl.h>` (angle-bracket, Apple SDK
// path) unmodified in its original .mm/.h files (we do not touch those — CLAUDE.md). Under
// Emscripten there is no OpenGLES framework, so this directory is added to the include path
// (web/CMakeLists.txt, BEFORE any system GL headers) purely so that literal include
// resolves — to our D2 shim instead. Angle-bracket includes are safe to redirect this way
// (unlike quoted includes, which always check the including file's own directory first —
// see docs/archive/PORT-STATUS-2026-08-13.md "Design decision: header shadowing" for why this only works for
// angle-bracket Apple-framework-style includes, not the project's own quoted "X.h" includes).
#ifndef EDEN_TRAMPOLINE_OPENGLES_ES1_GL_H
#define EDEN_TRAMPOLINE_OPENGLES_ES1_GL_H
#include "../../../gl_es1_shim.h"
#endif
