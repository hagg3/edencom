// <GLES/gl.h> for the NATIVE target — Phase N Stage 1
// (WORKING/native-migration-plan-2026-09-04.md).
//
// web/src/shim/gl/gl_es1_shim.h includes <GLES/gl.h> to get the GL *types* and the ES 1.1 *enum*
// surface; under Emscripten that resolves to emsdk's real Khronos ES1 header. There is no such
// header on a desktop toolchain, so this directory is put on the native target's include path
// (BEFORE any system GL header) and supplies the same two things over desktop GL instead:
//
//   1. the desktop GL core-profile header, which provides every GL type (GLenum/GLfloat/
//      GLubyte/GLsizeiptr/...) and every enum ES1 shares with GL 3.2 core, plus the real
//      entry points gl_fixed_function.cpp forwards to (glDrawArrays, glBufferData, ...);
//   2. the ES1-only enums core profile dropped, listed one-for-one below.
//
// It deliberately declares NO functions. gl_es1_shim.h already declares every ES1 fixed-function
// prototype it implements (glMatrixMode, glFogf, glLightfv, the client-array calls, ...) — that
// was verified before writing this file, and it is why nothing here has to redeclare them. If a
// future engine file calls an ES1 entry point neither header declares, the honest fix is to add
// it to gl_es1_shim.h (which serves BOTH targets), not to grow this file into a second ES1 API.
//
// WHY NOT VENDOR KHRONOS' ES1 HEADER: it declares the whole ES1 entry-point set, including calls
// this shim does not implement, so the link would only fail once something actually called one —
// i.e. at run time on a device. An enum-only header keeps "the shim does not implement that"
// a COMPILE error.
#ifndef EDEN_NATIVE_GLES_GL_H
#define EDEN_NATIVE_GLES_GL_H

#if defined(EDEN_PLATFORM_IOS)
// --- iOS (Phase N Stage 4.1) -----------------------------------------------------------------
// The one Apple platform that never deprecated GL and never had desktop GL either: iOS ships
// OpenGL ES, and <OpenGLES/ES3/gl.h> is the ES 3.0 surface this shim is written against on WEB
// (WebGL 2 is ES 3.0). So this branch is the closest of the four to the build that has had the
// most hours on it — the entry-point set gl_fixed_function.cpp forwards to is the same one
// emsdk resolves, which is why the file needs no iOS #ifdef of its own beyond the shader
// dialect (see eden_gl_preamble()).
//
// FRAMEWORK LOOKUP, and why this resolves to Apple's header rather than looping back here: the
// port shadows <OpenGLES/ES1/gl.h> and <OpenGLES/ES1/glext.h> (web/src/shim/gl/framework/) so
// the ENGINE's ES1 includes reach the shim, and it shadows nothing under ES3/ — so this include
// falls through the -I directories and is answered by the real OpenGLES.framework.
//
// GL_SILENCE_DEPRECATION for the same reason as the macOS branch below: Apple marks the whole
// OpenGLES framework deprecated in favour of Metal. It is deprecated, not removed, and it is
// present on every device this stage targets (A7 and up, iOS 12+).
#define GL_SILENCE_DEPRECATION 1
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#elif defined(__APPLE__)
// macOS ships OpenGL 4.1 core; <OpenGL/gl3.h> declares the 3.2-core subset statically, which is
// everything this shim's draw path uses (no loader needed — that is the one genuine convenience
// of the platform Apple has otherwise deprecated GL on). GL_SILENCE_DEPRECATION keeps the
// build's warning output readable; the deprecation is real and is what Stage 3+ (Metal, or
// MoltenGL) would answer, not something to hide from.
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#else
// --- Linux and Windows (Phase N Stage 3.2) ---------------------------------------------------
// Stage 1 left a hard `#error` here saying "picking a loader is a decision, not a default". The
// decision, with its reasoning, is in gl_loader_native.h: an explicit 61-entry-point table
// resolved through SDL_GL_GetProcAddress, rather than glad/GLEW/epoxy. Short version — Windows'
// `opengl32.dll` exports GL 1.1 and nothing else, so a loader is not optional there; SDL3 is
// already a hard dependency and already knows how to ask each platform; and an explicit list
// keeps this header's defining property, that an entry point the shim does not implement is a
// COMPILE error rather than a run-time one.
//
// TWO THINGS MUST HAPPEN BEFORE SDL's GL HEADER IS INCLUDED, both Windows-only and both
// load-bearing:
//
//  1. **`APIENTRY` must already be defined, because that is what stops `<windows.h>` from being
//     dragged into every engine translation unit.** SDL_opengl.h includes it purely to obtain
//     `APIENTRY` (`#if defined(_WIN32) && !defined(APIENTRY)`), and this header is reached from
//     Classes/Camera.h, World.h and Graphics.h — i.e. from most of the engine. Pulling windows.h
//     in there would collide `BOOL` (windows.h `int` vs Objective-C `signed char`) among many
//     other names, in files that have no business seeing the Win32 API. Defining the one macro it
//     wants is the whole fix.
//  2. **`GL_NO_STDCALL`**, which makes SDL's `GLAPIENTRY` empty on MinGW. On x86-64 Windows there
//     is only one calling convention and `__stdcall` is ignored, so this changes no code — it
//     makes the declarations, the shim's own definitions and gl_loader_native.h's function-pointer
//     typedefs agree *syntactically*, which is what keeps a future 32-bit build from being a
//     silent stack-corruption bug instead of a compile error. This target is x86-64 only; a 32-bit
//     Windows build would need this reconsidered, not just re-flagged.
//
// Both are set here rather than in CMake so that anyone reading this file sees why they exist.
#if defined(_WIN32)
#  ifndef APIENTRY
#    define APIENTRY
#  endif
#  ifndef GL_NO_STDCALL
#    define GL_NO_STDCALL 1
#  endif
#endif
#include "../../gl_loader_native.h"
#endif

// ---------------------------------------------------------------------------------------------
// ES 1.1 enums that OpenGL core profile removed. Values are the canonical Khronos ones (same
// numbers ES1, GL 1.x compatibility and every vendor's header have carried since OpenGL 1.1), so
// nothing has to be renumbered if this is ever compared against a real ES1 header.
//
// Every entry below is here because some file the native target compiles NAMES it — engine code
// under Classes/, or the shim's own state capture. Naming a token is not the same as it working:
// the shim's job is to translate these into shader/uniform state (see gl_fixed_function.cpp), and
// a token that only exists so a file compiles is marked as such.
// ---------------------------------------------------------------------------------------------

// Alpha test (GLES2+ and core profile both deleted it; the shim emulates it with `discard`).
#ifndef GL_ALPHA_TEST
#define GL_ALPHA_TEST                  0x0BC0
#endif

// Fog modes. The engine only ever selects GL_LINEAR (Graphics.mm's setZFAR band), but Fog.mm
// names the exponential ones.
#ifndef GL_EXP
#define GL_EXP                         0x0800
#endif
#ifndef GL_EXP2
#define GL_EXP2                        0x0801
#endif

// Fixed-function lighting/material state. GL_COLOR_MATERIAL in particular is load-bearing:
// Graphics.mm enables it globally, which is why the skinning shader's whole lighting result is
// one scalar times the current colour (see kSkinVertexShader's comment).
#ifndef GL_COLOR_MATERIAL
#define GL_COLOR_MATERIAL              0x0B57
#endif
#ifndef GL_NORMALIZE
#define GL_NORMALIZE                   0x0BA1
#endif
#ifndef GL_RESCALE_NORMAL
#define GL_RESCALE_NORMAL              0x803A
#endif

// Hints. Both are accepted-and-ignored by the shim (a hint has no observable semantics).
#ifndef GL_FOG_HINT
#define GL_FOG_HINT                    0x0C54
#endif
#ifndef GL_PERSPECTIVE_CORRECTION_HINT
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#endif
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP             0x8191
#endif

// Single-channel texture formats. Core profile replaced these with GL_RED/GL_RG + a swizzle.
// Texture2D_web.mm's decoder only ever produces RGB/RGBA (stb_image), so these are compile
// fodder for the format enum in Texture2D.h, not a path this target takes.
#ifndef GL_LUMINANCE
#define GL_LUMINANCE                   0x1909
#endif
#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA             0x190A
#endif

// glGetError values ES1 has and core profile dropped. Classes/error.c switches on all three.
#ifndef GL_STACK_OVERFLOW
#define GL_STACK_OVERFLOW              0x0503
#endif
#ifndef GL_STACK_UNDERFLOW
#define GL_STACK_UNDERFLOW             0x0504
#endif
#ifndef GL_TABLE_TOO_LARGE
#define GL_TABLE_TOO_LARGE             0x8031
#endif

// User clip planes. Named by Classes/error.c / the PVRT SDK; the engine never enables one.
#ifndef GL_MAX_CLIP_PLANES
#define GL_MAX_CLIP_PLANES             0x0D32
#endif
#ifndef GL_CLIP_PLANE0
#define GL_CLIP_PLANE0                 0x3000
#define GL_CLIP_PLANE1                 0x3001
#define GL_CLIP_PLANE2                 0x3002
#define GL_CLIP_PLANE3                 0x3003
#define GL_CLIP_PLANE4                 0x3004
#define GL_CLIP_PLANE5                 0x3005
#endif

// The GL_COMBINE texture-environment surface. The engine uses GL_MODULATE and GL_DECAL (both
// still in core headers as enums); the rest of the combiner vocabulary is named by the vendored
// PVRT tools and by Classes/error.c's enum dumps.
#ifndef GL_COMBINE
#define GL_COMBINE                     0x8570
#define GL_COMBINE_RGB                 0x8571
#define GL_COMBINE_ALPHA               0x8572
#define GL_RGB_SCALE                   0x8573
#define GL_ADD_SIGNED                  0x8574
#define GL_INTERPOLATE                 0x8575
#define GL_SUBTRACT                    0x84E7
#define GL_CONSTANT                    0x8576
#define GL_PRIMARY_COLOR               0x8577
#define GL_PREVIOUS                    0x8578
#define GL_SOURCE0_RGB                 0x8580
#define GL_SOURCE1_RGB                 0x8581
#define GL_SOURCE2_RGB                 0x8582
#define GL_SOURCE0_ALPHA               0x8588
#define GL_SOURCE1_ALPHA               0x8589
#define GL_SOURCE2_ALPHA               0x858A
#define GL_OPERAND0_RGB                0x8590
#define GL_OPERAND1_RGB                0x8591
#define GL_OPERAND2_RGB                0x8592
#define GL_OPERAND0_ALPHA              0x8598
#define GL_OPERAND1_ALPHA              0x8599
#define GL_OPERAND2_ALPHA              0x859A
#define GL_DOT3_RGB                    0x86AE
#define GL_DOT3_RGBA                   0x86AF
#endif

// ES1 fixed-point type. Named by glFogx's signature (gl_es1_shim.h GROUP 3) and by the PVRT
// fixed-point headers; no engine call site passes one.
#ifndef GL_FIXED
#define GL_FIXED                       0x140C
#endif
typedef int GLfixed;
typedef int GLclampx;

// The two ES1 entry points only the vendored PVRT SDK calls — declared here rather than in
// gl_es1_shim.h because that header's inventory is deliberately "what the ENGINE calls". Defined
// in native/src/shim/gl/gl_es1_extras_native.cpp; see that file for what each one actually does.
#ifdef __cplusplus
extern "C" {
#endif
void glClientActiveTexture(GLenum texture);
void glTexEnvf(GLenum target, GLenum pname, GLfloat param);
#ifdef __cplusplus
}
#endif

#endif  // EDEN_NATIVE_GLES_GL_H
