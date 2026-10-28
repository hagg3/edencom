// <GLES/gl.h> for the NATIVE target — the platform's real GL, for the GL backend (port/src/render/gl/)
// only. Phase N Stage 1 wrote it for the ES 1.1 shim (WORKING/native-migration-plan-2026-09-04.md);
// G.5b-7 deleted the shim and moved this directory to port/src/render/gl/framework/. Engine code reaches
// <OpenGLES/ES1/gl.h> (beside this, types only) and never this file.
//
// It supplies, per platform:
//   1. the GL header with every type, every enum and the entry points the backend calls
//      (iOS: <OpenGLES/ES3/gl.h>; macOS: <OpenGL/gl3.h>; Linux/Windows: SDL_opengl.h through
//      gl_loader_native.h's explicit entry-point table);
//   2. the few ES 1.1 enums core profile dropped that the backend still names, listed below.
//
// It declares NO functions of its own: an entry point nobody loads stays a COMPILE error (Linux and
// Windows get theirs from gl_loader_native.h's list).
#ifndef EDEN_NATIVE_GLES_GL_H
#define EDEN_NATIVE_GLES_GL_H

#if defined(EDEN_PLATFORM_IOS)
// --- iOS (Phase N Stage 4.1): OpenGL ES 3.0, the same surface the web build had through WebGL 2.
// This include resolves to Apple's header, not back here: the port shadows only <OpenGLES/ES1/...>.
// GL_SILENCE_DEPRECATION: Apple marks the OpenGLES framework deprecated in favour of Metal; it is
// present on every device this port targets.
#define GL_SILENCE_DEPRECATION 1
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#elif defined(__APPLE__)
// macOS ships OpenGL 4.1 core; <OpenGL/gl3.h> declares the 3.2-core subset statically, which is
// everything the backend uses (no loader needed — that is the one genuine convenience
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
// keeps this header's defining property, that an entry point nobody loads is a
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
//     makes the declarations and gl_loader_native.h's function-pointer
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
// ES 1.1 enums OpenGL core profile removed that the backend still names. Khronos values.
// (Until G.5b-7 this block also carried the fixed-function vocabulary the engine and the shim named:
// alpha test, fog modes, lighting, hints, clip planes, the combiner; nothing names them now.)
// ---------------------------------------------------------------------------------------------

// gl_textures.cpp: ES 1.1's GL_GENERATE_MIPMAP is the "mipmaps wanted" request it turns into a
// deferred glGenerateMipmap, and L/A/LA uploads (Q.4) become R8/RG8 + swizzle on desktop core.
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP             0x8191
#endif
#ifndef GL_LUMINANCE
#define GL_LUMINANCE                   0x1909
#endif
#ifndef GL_LUMINANCE_ALPHA
#define GL_LUMINANCE_ALPHA             0x190A
#endif

// ES1's fixed-point attribute type: gl_stream.cpp's type-size table names it.
#ifndef GL_FIXED
#define GL_FIXED                       0x140C
#endif

#endif  // EDEN_NATIVE_GLES_GL_H
