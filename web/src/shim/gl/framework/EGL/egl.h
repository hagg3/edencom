// <EGL/egl.h> — EGL does not exist on the web, and this port never wants it to.
//
// It is reached only through Classes/PVRTglesExt.h's non-Apple branch (see GLES/gl.h), which uses
// exactly one EGL symbol: `#define PVRGetProcAddress(x) eglGetProcAddress(#x)`, used to populate
// CPVRTglesExt's extension function-pointer table.
//
// eglGetProcAddress returning NULL here is the CORRECT behavior, not a shortcut: WebGL genuinely
// has no GL_OES_matrix_palette, so every entry point PVRT looks up genuinely is unavailable. The
// engine already handles that — Classes/Model.mm's LoadModels() checks
// `CPVRTglesExt::IsGLExtensionSupported("GL_OES_matrix_palette")` and returns false if it is
// missing.
//
// *** CONSEQUENCE, and it is a real gameplay gap to carry forward: with no matrix palette,
// LoadModels() bails and NO CREATURE MODELS LOAD. *** Hardware matrix-palette skinning has no
// WebGL equivalent at all; the animated creatures need skinning moved to the CPU or into a
// vertex shader. That is renderer work (web-port-plan.md Stage P2 for a first correct frame,
// Stage R7 for the WebGL2 backend that would do it properly) — recorded here rather than
// papered over, since a build that silently has no creatures would otherwise look like a
// content-loading bug.
#ifndef EDEN_TRAMPOLINE_EGL_EGL_H
#define EDEN_TRAMPOLINE_EGL_EGL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*__eglMustCastToProperFunctionPointerType)(void);

// Always NULL — see this header's comment.
inline __eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *procname) {
  (void)procname;
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif
