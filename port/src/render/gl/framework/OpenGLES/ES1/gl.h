// <OpenGLES/ES1/gl.h> for the ENGINE — G.5b-7 (2026-10-28): GL's TYPES and a few enum VALUES, and no
// function at all. The engine's own headers include this Apple path unmodified (Classes/Graphics.h,
// World.h, Vector.h, glu.h, ...); this directory is on the include path ahead of any system header, so the
// angle-bracket include lands here. Until G.5b-7 it led to the ES 1.1 shim's header (gl_es1_shim.h),
// which declared and implemented the whole fixed-function API; the shim is gone and the engine draws
// through R:: (Classes/Renderer.h), so a GL call in an engine file is now a compile error, and
// port/tools/gl-call-gate.py catches the rest. The GL backend (port/src/render/gl/) includes the
// platform's real GL through <GLES/gl.h> instead; no file needs both.
//
// The typedefs are the platform headers' own types (macOS's OpenGL/gltypes.h, iOS's OpenGLES/gltypes.h,
// SDL_opengl.h), so a repeat definition is the same type and stays legal. GLdouble/GLclampd are glu.h's.
#ifndef EDEN_ENGINE_OPENGLES_ES1_GL_H
#define EDEN_ENGINE_OPENGLES_ES1_GL_H

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef signed char    GLbyte;
typedef short          GLshort;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLubyte;
typedef unsigned short GLushort;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef float          GLclampf;
typedef void           GLvoid;
typedef int            GLfixed;
typedef int            GLclampx;

// Named by live engine code: glu's GLboolean returns (project.c, registry.c) and error.c's
// gluErrorString table. Khronos values.
#ifndef GL_FALSE
#define GL_FALSE                             0
#define GL_TRUE                              1
#endif
#ifndef GL_NO_ERROR
#define GL_NO_ERROR                          0
#define GL_INVALID_ENUM                      0x0500
#define GL_INVALID_VALUE                     0x0501
#define GL_INVALID_OPERATION                 0x0502
#define GL_STACK_OVERFLOW                    0x0503
#define GL_STACK_UNDERFLOW                   0x0504
#define GL_OUT_OF_MEMORY                     0x0505
#endif
#ifndef GL_INVALID_FRAMEBUFFER_OPERATION_EXT
#define GL_INVALID_FRAMEBUFFER_OPERATION_EXT 0x0506
#endif

#endif  // EDEN_ENGINE_OPENGLES_ES1_GL_H
