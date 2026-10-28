// gl_textures.h — texture uploads behind R::texCreate / texSampling (G.5b-0: lifted out of the ES 1.1
// shim unchanged; the shim's own callers went with it in G.5b-7). Every rule lives here once: ES 1.1's
// GL_GENERATE_MIPMAP as a deferred glGenerateMipmap, NPOT (Q.2), L/A/LA as R8/RG8 + swizzle on desktop
// core (Q.4), glFlush after each upload (N.4.9), the byte accounting, --mem-trace=2's probe and
// --tex-exp=. Both entry points act on the texture bound to unit 0 NOW (bindTextureNow first).
#ifndef EDEN_RENDER_GL_TEXTURES_H
#define EDEN_RENDER_GL_TEXTURES_H

#include "gl_state.h"

namespace rgl {

// A context is live in both.
void texParameteri(GLenum target, GLenum pname, GLint param);
void texImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
                GLint border, GLenum format, GLenum type, const void* pixels);

}  // namespace rgl

#endif  // EDEN_RENDER_GL_TEXTURES_H
