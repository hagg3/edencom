// Trampoline header for `<OpenGLES/ES1/glext.h>` — see gl.h in this directory for why this
// pattern is safe (angle-bracket include redirection). All the ES1-extension surface the
// engine actually calls (OES/EXT names) is already declared in gl_es1_shim.h GROUP 7
// (occlusion queries) — everything else under this include name in the original codebase is
// PVRT-SDK-internal (see gl_es1_shim.h's "EXCLUDED from this shim" note) and is not exercised
// by any engine (non-PVRT) file.
#ifndef EDEN_TRAMPOLINE_OPENGLES_ES1_GLEXT_H
#define EDEN_TRAMPOLINE_OPENGLES_ES1_GLEXT_H
#include "../../../gl_es1_shim.h"

// --- GL_OES_matrix_palette tokens ---------------------------------------------------------
// These are TOKENS ONLY, deliberately: Classes/Model.mm names them at compile time in its
// hardware-skinning path (glEnableClientState(GL_MATRIX_INDEX_ARRAY_OES),
// glEnable/glMatrixMode(GL_MATRIX_PALETTE_OES), …), so the file cannot compile without them —
// but nothing here makes the extension WORK, and nothing should.
//
// WebGL has no matrix-palette skinning. The engine detects that correctly on its own:
// Classes/Model.mm's LoadModels() early-returns when
// IsGLExtensionSupported("GL_OES_matrix_palette") is false, which it will be, because
// framework/EGL/egl.h's eglGetProcAddress returns NULL and the shim's GL_EXTENSIONS string does
// not advertise it. So this code is unreachable at runtime rather than broken at runtime — the
// distinction that matters when reading a stack trace later.
//
// See framework/EGL/egl.h for the consequence to carry forward (creature models do not load
// until skinning moves to the CPU or a vertex shader — Stage P2/R7). Values are the canonical
// Khronos ones, so if a later stage does implement skinning against these names, nothing has to
// be renumbered.
#define GL_MATRIX_PALETTE_OES              0x8840
#define GL_MAX_PALETTE_MATRICES_OES        0x8842
#define GL_MAX_VERTEX_UNITS_OES            0x86A4
#define GL_CURRENT_PALETTE_MATRIX_OES      0x8843
#define GL_MATRIX_INDEX_ARRAY_OES          0x8844
#define GL_WEIGHT_ARRAY_OES                0x86AD

#endif
