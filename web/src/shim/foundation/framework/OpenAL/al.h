// <OpenAL/al.h> — trampoline to whatever OpenAL the target has, or to nothing at all.
//
// Apple puts OpenAL under <OpenAL/…>; everyone else uses <AL/…>. Same trick as
// framework/OpenGLES/ES1/gl.h. The header exists because Classes/Sound.h imports it
// unconditionally and Classes/Resources.h imports Sound.h, so it is parsed by most of the engine.
//
// *** NOTHING IN THIS PORT CALLS OPENAL ON ANY TARGET. *** Verified rather than assumed: `nm -u`
// over every object file in the native build reports not one `_al*` or `_alc*` undefined symbol.
// `Classes/Sound.{h,mm}` is 2010-era dead code — it is not in the engine source list on either
// target, and audio goes through CocosDenshion's SimpleAudioEngine, which this port reimplements.
// So the ONLY requirement on this header is that Sound.h PARSES, and the only three names it
// needs are ALCcontext, ALCdevice and ALfloat.
#ifndef EDEN_SHIM_OPENAL_AL_H
#define EDEN_SHIM_OPENAL_AL_H

#if defined(__APPLE__)
// A real system framework, and this directory sits ahead of the SDK's on the include path, so
// `#include_next` continues the search past here and finds
// /System/…/OpenAL.framework/Headers/. Deprecated by Apple; that is fine, nothing calls it.
#include_next <OpenAL/al.h>

#elif defined(__EMSCRIPTEN__)
// emsdk ships a real, Web-Audio-backed implementation at system/include/AL/.
#include <AL/al.h>

#else
// Phase N Stage 3.2 — LINUX AND WINDOWS, where there is no OpenAL header unless somebody installs
// one. The previous `#else` here assumed "not Apple" meant "emsdk", which was true when only two
// targets existed; on the Linux runner it produced `fatal error: 'AL/al.h' file not found` in
// every engine translation unit that reaches Resources.h.
//
// The fix is a TYPE-ONLY STUB rather than an openal-dev dependency, and that is the same call
// framework/AudioToolbox and framework/AVFoundation already make: adding a real library to the
// build so that a dead header parses would make OpenAL a shipping dependency of a program that
// never calls it. If something ever does call it, this stub fails at LINK time with the symbol
// named, which is the loud failure — not at run time.
typedef char           ALboolean;
typedef char           ALchar;
typedef signed char    ALbyte;
typedef unsigned char  ALubyte;
typedef short          ALshort;
typedef unsigned short ALushort;
typedef int            ALint;
typedef unsigned int   ALuint;
typedef int            ALsizei;
typedef int            ALenum;
typedef float          ALfloat;
typedef double         ALdouble;
typedef void           ALvoid;
#endif

#endif
