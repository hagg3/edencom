// <GLES/glext.h> — the non-Apple spelling of the ES1 extension header, which the vendored PVRT
// SDK asks for: Classes/PVRTglesExt.h branches on `#if defined(__APPLE__)` and, since that is
// undefined under Emscripten, takes the else-branch requesting <EGL/egl.h> + <GLES/gl.h> +
// <GLES/glext.h>. (Defining __APPLE__ to force the other branch was rejected — it would change
// how every system header behaves, to fix one include line.)
//
// NOTE THE ASYMMETRY, it is deliberate: there is NO framework/GLES/gl.h beside this file. That
// name must keep resolving to EMSCRIPTEN's real ES1 header, because ../../gl_es1_shim.h includes
// <GLES/gl.h> itself to get GLenum/GLfloat and the ES1 enum values. Shadowing it here made the
// include circular — the shim pulled in this directory instead of the system header, hit its own
// already-defined include guard, and every GL type in the shim's own declarations came back
// undefined. Only the extension header is shadowed, and only to add the tokens below.
#ifndef EDEN_TRAMPOLINE_GLES_GLEXT_H
#define EDEN_TRAMPOLINE_GLES_GLEXT_H
#include "../OpenGLES/ES1/glext.h"
#endif
