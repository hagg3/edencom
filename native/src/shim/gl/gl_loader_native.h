// gl_loader_native.h — the desktop-GL entry-point loader for the NON-APPLE native targets
// (Phase N Stage 3.2, WORKING/phase-n-stage3plus-plan-2026-09-05.md).
//
// WHY THIS FILE EXISTS AT ALL, since macOS needs nothing like it: **`opengl32.dll` exports
// OpenGL 1.1 and nothing else.** Every entry point this port uses from GL 1.5 upward — the whole
// buffer-object and shader surface, which is most of the renderer — has to be fetched at run time
// through `wglGetProcAddress` on Windows. Linux's libGL does export the modern symbols in
// practice, but relying on that would give Windows its own code path for no gain, and the plan's
// whole reason for doing Linux first is to change ONE variable at a time. So both non-Apple legs
// load identically, through SDL (`SDL_GL_GetProcAddress`, which on Windows already knows to fall
// back to `GetProcAddress(opengl32)` for the 1.1 names that `wglGetProcAddress` refuses).
//
// THE LIST IS EXPLICIT, AND THAT IS THE POINT. It is not a hand-guessed set: it is the exact
// symbol set the macOS link leaves undefined, extracted mechanically —
//
//     nm -u  $(find native/build/CMakeFiles -name '*.o') | grep -oE '^_gl[A-Za-z0-9]+'
//     nm -g --defined-only <same> | grep -oE ' _gl[A-Za-z0-9]+$'
//     comm -23 <undefined> <defined>          # -> exactly the 61 below
//
// — so it cannot silently disagree with what the shim actually calls. Re-run that after adding a
// GL call and add the new name here; the failure mode if you forget is a link error naming the
// symbol, which is the good kind. This preserves the property
// `native/src/shim/gl/framework/GLES/gl.h` was written for: **an entry point the shim does not
// implement is a COMPILE error, not a run-time one.**
//
// HOW THE RENAME WORKS, and why it is a macro rather than same-named function pointers: SDL's
// bundled GL header declares real prototypes for the 26 GL-1.1 names, so `extern PFN glClear;`
// would be "redefinition as a different kind of symbol". Including that header FIRST and then
// macro-renaming the call sites is legal and leaves the (now unused) prototypes alone. Nothing
// else in this build includes a GL header, so no later declaration can collide.
#ifndef EDEN_NATIVE_GL_LOADER_H
#define EDEN_NATIVE_GL_LOADER_H

#if !defined(__APPLE__)

// Types, enums, and the GL 1.1 prototypes — SDL3 bundles Mesa's GL 1.1 header plus Khronos'
// glext, which together supply GLchar/GLsizeiptr/GLintptr and every enum above 1.1. Vendoring
// Khronos' own headers would have worked too; this is one fewer thing in the tree, and SDL3 is
// already a hard dependency of this target.
#include <SDL3/SDL_opengl.h>

// The whole surface, in one list. Order is `nm`'s (alphabetical); signatures are the Khronos ones.
// EDEN_GL_FN(return type, name, parenthesised parameter list)
#define EDEN_GL_ENTRY_POINTS(X)                                                                  \
  X(void,           glActiveTexture,           (GLenum texture))                                 \
  X(void,           glAttachShader,            (GLuint program, GLuint shader))                  \
  X(void,           glBindAttribLocation,      (GLuint program, GLuint index, const GLchar *name))\
  X(void,           glBindBuffer,              (GLenum target, GLuint buffer))                   \
  X(void,           glBindFramebuffer,         (GLenum target, GLuint framebuffer))              \
  X(void,           glBindTexture,             (GLenum target, GLuint texture))                  \
  X(void,           glBindVertexArray,         (GLuint array))                                   \
  X(void,           glBlendFunc,               (GLenum sfactor, GLenum dfactor))                 \
  X(void,           glBufferData,              (GLenum target, GLsizeiptr size, const void *data, \
                                                GLenum usage))                                   \
  X(void,           glBufferSubData,           (GLenum target, GLintptr offset, GLsizeiptr size, \
                                                const void *data))                               \
  X(void,           glClear,                   (GLbitfield mask))                                \
  X(void,           glClearColor,              (GLfloat r, GLfloat g, GLfloat b, GLfloat a))     \
  X(void,           glCompileShader,           (GLuint shader))                                  \
  X(void,           glCompressedTexImage2D,    (GLenum target, GLint level, GLenum internalformat,\
                                                GLsizei width, GLsizei height, GLint border,     \
                                                GLsizei imageSize, const void *data))            \
  X(GLuint,         glCreateProgram,           (void))                                           \
  X(GLuint,         glCreateShader,            (GLenum type))                                    \
  X(void,           glCullFace,                (GLenum mode))                                    \
  X(void,           glDeleteBuffers,           (GLsizei n, const GLuint *buffers))               \
  X(void,           glDeleteProgram,           (GLuint program))                                 \
  X(void,           glDeleteShader,            (GLuint shader))                                  \
  X(void,           glDeleteTextures,          (GLsizei n, const GLuint *textures))              \
  X(void,           glDeleteVertexArrays,      (GLsizei n, const GLuint *arrays))                \
  X(void,           glDepthMask,               (GLboolean flag))                                 \
  X(void,           glDisable,                 (GLenum cap))                                     \
  X(void,           glDisableVertexAttribArray,(GLuint index))                                   \
  X(void,           glDrawArrays,              (GLenum mode, GLint first, GLsizei count))        \
  X(void,           glDrawElements,            (GLenum mode, GLsizei count, GLenum type,         \
                                                const void *indices))                            \
  X(void,           glEnable,                  (GLenum cap))                                     \
  X(void,           glEnableVertexAttribArray, (GLuint index))                                   \
  X(void,           glFrontFace,               (GLenum mode))                                    \
  X(void,           glGenBuffers,              (GLsizei n, GLuint *buffers))                     \
  X(void,           glGenerateMipmap,          (GLenum target))                                  \
  X(void,           glGenTextures,             (GLsizei n, GLuint *textures))                    \
  X(void,           glGenVertexArrays,         (GLsizei n, GLuint *arrays))                      \
  X(GLenum,         glGetError,                (void))                                           \
  X(void,           glGetFloatv,               (GLenum pname, GLfloat *params))                  \
  X(void,           glGetIntegerv,             (GLenum pname, GLint *params))                    \
  X(void,           glGetProgramInfoLog,       (GLuint program, GLsizei bufSize, GLsizei *length,\
                                                GLchar *infoLog))                                \
  X(void,           glGetProgramiv,            (GLuint program, GLenum pname, GLint *params))    \
  X(void,           glGetShaderInfoLog,        (GLuint shader, GLsizei bufSize, GLsizei *length, \
                                                GLchar *infoLog))                                \
  X(void,           glGetShaderiv,             (GLuint shader, GLenum pname, GLint *params))     \
  X(const GLubyte*, glGetString,               (GLenum name))                                    \
  X(GLint,          glGetUniformLocation,      (GLuint program, const GLchar *name))             \
  X(void,           glHint,                    (GLenum target, GLenum mode))                     \
  X(void,           glLineWidth,               (GLfloat width))                                  \
  X(void,           glLinkProgram,             (GLuint program))                                 \
  X(void,           glPixelStorei,             (GLenum pname, GLint param))                      \
  X(void,           glPolygonOffset,           (GLfloat factor, GLfloat units))                  \
  X(void,           glReadPixels,              (GLint x, GLint y, GLsizei width, GLsizei height, \
                                                GLenum format, GLenum type, void *pixels))       \
  X(void,           glShaderSource,            (GLuint shader, GLsizei count,                    \
                                                const GLchar *const *string, const GLint *length))\
  X(void,           glTexImage2D,              (GLenum target, GLint level, GLint internalformat,\
                                                GLsizei width, GLsizei height, GLint border,     \
                                                GLenum format, GLenum type, const void *pixels)) \
  X(void,           glTexParameterf,           (GLenum target, GLenum pname, GLfloat param))     \
  X(void,           glTexParameteri,           (GLenum target, GLenum pname, GLint param))       \
  X(void,           glUniform1f,               (GLint location, GLfloat v0))                     \
  X(void,           glUniform1i,               (GLint location, GLint v0))                       \
  X(void,           glUniform3fv,              (GLint location, GLsizei count, const GLfloat *v))\
  X(void,           glUniform4fv,              (GLint location, GLsizei count, const GLfloat *v))\
  X(void,           glUniformMatrix4fv,        (GLint location, GLsizei count, GLboolean transpose,\
                                                const GLfloat *value))                           \
  X(void,           glUseProgram,              (GLuint program))                                 \
  X(void,           glVertexAttribPointer,     (GLuint index, GLint size, GLenum type,           \
                                                GLboolean normalized, GLsizei stride,            \
                                                const void *pointer))                            \
  X(void,           glViewport,                (GLint x, GLint y, GLsizei width, GLsizei height))

#ifdef __cplusplus
extern "C" {
#endif

// The GL calling convention is `__stdcall` on 32-bit Windows and plain on everything else. SDL's
// header spells it APIENTRY; use it where it exists so a future 32-bit Windows build is not a
// silent stack-corruption bug (the symptom would be a crash on RETURN from a GL call, which reads
// as a driver fault, not as a calling-convention one).
#if defined(APIENTRY)
#define EDEN_GL_APIENTRY APIENTRY
#else
#define EDEN_GL_APIENTRY
#endif

// One `eden_glfp_<name>` function pointer per entry point.
#define EDEN_GL_DECLARE(ret, name, params) \
  typedef ret(EDEN_GL_APIENTRY *eden_glfp_t_##name) params; \
  extern eden_glfp_t_##name eden_glfp_##name;
EDEN_GL_ENTRY_POINTS(EDEN_GL_DECLARE)
#undef EDEN_GL_DECLARE

// Resolves every pointer above through SDL_GL_GetProcAddress. MUST be called with a current GL
// context — on Windows `wglGetProcAddress` returns NULL without one, so calling this too early
// yields a loader that "succeeds" with 61 null pointers on some drivers and fails cleanly on
// others. gl_context_native.cpp calls it immediately after SDL_GL_MakeCurrent and nowhere else.
//
// Returns the number of entry points that did NOT resolve, and prints each missing name. Zero is
// the only acceptable answer; the caller treats anything else as a fatal context error, because
// the alternative is a null call somewhere inside the first frame.
int eden_native_gl_load_entry_points(void);

// How many entry points the X-macro list holds. gl_loader_native.cpp static_asserts this against
// the rename list below -- see the note there.
#define EDEN_GL_COUNT_ONE(ret, name, params) +1
enum { EDEN_GL_ENTRY_POINT_COUNT = 0 EDEN_GL_ENTRY_POINTS(EDEN_GL_COUNT_ONE) };
#undef EDEN_GL_COUNT_ONE

#ifdef __cplusplus
}
#endif

// The rename. After this point `glDrawArrays(...)` in the shim and in Classes/ compiles to a call
// through `eden_glfp_glDrawArrays`. Deliberately AFTER the declarations above, so the typedefs and
// the extern lines can still spell the real names.
//
// THIS LIST DUPLICATES THE ONE ABOVE, and the C preprocessor gives no way to avoid that (a macro
// cannot emit a `#define`). The duplication is guarded rather than trusted:
//   * an entry here with no entry above is a compile error (the pointer does not exist);
//   * an entry above with none here is worse -- it would be SILENT on Linux, whose libGL does
//     export the modern symbols, and a link error only on Windows. So the count is asserted in
//     gl_loader_native.cpp, where both lists are visible at once.
// Keep them in the same (alphabetical) order and add to both.

#define glActiveTexture            eden_glfp_glActiveTexture
#define glAttachShader             eden_glfp_glAttachShader
#define glBindAttribLocation       eden_glfp_glBindAttribLocation
#define glBindBuffer               eden_glfp_glBindBuffer
#define glBindFramebuffer          eden_glfp_glBindFramebuffer
#define glBindTexture              eden_glfp_glBindTexture
#define glBindVertexArray          eden_glfp_glBindVertexArray
#define glBlendFunc                eden_glfp_glBlendFunc
#define glBufferData               eden_glfp_glBufferData
#define glBufferSubData            eden_glfp_glBufferSubData
#define glClear                    eden_glfp_glClear
#define glClearColor               eden_glfp_glClearColor
#define glCompileShader            eden_glfp_glCompileShader
#define glCompressedTexImage2D     eden_glfp_glCompressedTexImage2D
#define glCreateProgram            eden_glfp_glCreateProgram
#define glCreateShader             eden_glfp_glCreateShader
#define glCullFace                 eden_glfp_glCullFace
#define glDeleteBuffers            eden_glfp_glDeleteBuffers
#define glDeleteProgram            eden_glfp_glDeleteProgram
#define glDeleteShader             eden_glfp_glDeleteShader
#define glDeleteTextures           eden_glfp_glDeleteTextures
#define glDeleteVertexArrays       eden_glfp_glDeleteVertexArrays
#define glDepthMask                eden_glfp_glDepthMask
#define glDisable                  eden_glfp_glDisable
#define glDisableVertexAttribArray eden_glfp_glDisableVertexAttribArray
#define glDrawArrays               eden_glfp_glDrawArrays
#define glDrawElements             eden_glfp_glDrawElements
#define glEnable                   eden_glfp_glEnable
#define glEnableVertexAttribArray  eden_glfp_glEnableVertexAttribArray
#define glFrontFace                eden_glfp_glFrontFace
#define glGenBuffers               eden_glfp_glGenBuffers
#define glGenerateMipmap           eden_glfp_glGenerateMipmap
#define glGenTextures              eden_glfp_glGenTextures
#define glGenVertexArrays          eden_glfp_glGenVertexArrays
#define glGetError                 eden_glfp_glGetError
#define glGetFloatv                eden_glfp_glGetFloatv
#define glGetIntegerv              eden_glfp_glGetIntegerv
#define glGetProgramInfoLog        eden_glfp_glGetProgramInfoLog
#define glGetProgramiv             eden_glfp_glGetProgramiv
#define glGetShaderInfoLog         eden_glfp_glGetShaderInfoLog
#define glGetShaderiv              eden_glfp_glGetShaderiv
#define glGetString                eden_glfp_glGetString
#define glGetUniformLocation       eden_glfp_glGetUniformLocation
#define glHint                     eden_glfp_glHint
#define glLineWidth                eden_glfp_glLineWidth
#define glLinkProgram              eden_glfp_glLinkProgram
#define glPixelStorei              eden_glfp_glPixelStorei
#define glPolygonOffset            eden_glfp_glPolygonOffset
#define glReadPixels               eden_glfp_glReadPixels
#define glShaderSource             eden_glfp_glShaderSource
#define glTexImage2D               eden_glfp_glTexImage2D
#define glTexParameterf            eden_glfp_glTexParameterf
#define glTexParameteri            eden_glfp_glTexParameteri
#define glUniform1f                eden_glfp_glUniform1f
#define glUniform1i                eden_glfp_glUniform1i
#define glUniform3fv               eden_glfp_glUniform3fv
#define glUniform4fv               eden_glfp_glUniform4fv
#define glUniformMatrix4fv         eden_glfp_glUniformMatrix4fv
#define glUseProgram               eden_glfp_glUseProgram
#define glVertexAttribPointer      eden_glfp_glVertexAttribPointer
#define glViewport                 eden_glfp_glViewport

#endif  // !__APPLE__
#endif  // EDEN_NATIVE_GL_LOADER_H
